/* 
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/cpu/sh/sh4.c, Copyright (C) 2011, 2012, Stefano Teso.
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
 * IRQs
 * ====
 *
 * IRQ priorities are encoded in sh4.intc.irqs. Whenever an external IRQ
 * or on-chip IRQ should be raised or cleared, call sh4_set_irq_state.
 * It will update the sh4.intc.irqs table with the proper state.
 *
 * The interrupt priorities are either fixed (for external IRQs an
 * exceptions) or decided by the INTC settings; sh4_ireg_put will make
 * sure to update the sh4.intc.irqs priorities according to the INTC
 * configuration.
 */

/* TODO: propagate get/put and instruction errors to the main loop */
/* TODO: the exceptions but the MMU's and TRAPA (see "MMU"): the illegal
 * instructions, the FPU disabled, the address errors without the MMU */
/* TODO: handle FP exceptions and rounding mode */

#include "hl/core.h"
#include "hl/cpu.h"
#include "hl/profile.h"
#include "hl/state.h"

#include "sh4.h"
#include "sh4-ireg.h"
#include "sh4-x64.h"

/* Helper Macros */

#define _RN	((inst >> 8) & 15)
#define _RM	((inst >> 4) & 15)
#define _UIMM8	((uint32_t)(uint8_t) inst)
#define _SIMM8	((int32_t)(int8_t) inst)
#define _SIMM12 ((int32_t)((inst & 0xFFF) | ((inst & 0x800) ? 0xFFFFF000 : 0)))

#define PC	ctx->regs.pc
#define PR	ctx->regs.pr
#define SR	ctx->regs.sr
#define MAC	ctx->regs.mac.full
#define MACH	ctx->regs.mac.field.hi
#define MACL	ctx->regs.mac.field.lo
#define GBR	ctx->regs.gbr
#define VBR	ctx->regs.vbr
#define SSR	ctx->regs.ssr
#define SPC	ctx->regs.spc
#define DBR	ctx->regs.dbr
#define SGR	ctx->regs.sgr

#define R(n_)	ctx->regs.r[n_]
#define R0	R(0)
#define RN	R(_RN)
#define RM	R(_RM)
#define SP	R(15)

#define RBANK(n_) ctx->regs.rbank[n_]

#define T	SR.bit.t
#define S	SR.bit.s
#define Q	SR.bit.q
#define M	SR.bit.m

#define FPSCR	ctx->regs.fpscr
#define FPUL	ctx->regs.fpul

/* DRn is FRn (its high half: sign, exponent) and FRn+1 (its low half), the
 * host's double the pair in the other order: the singles at n ^ 1 (and the
 * 64-bit moves swap their words, MOVE64). A double built of two fmov.s, as
 * the compilers' unsigned division does (PHARRIER's at 0C10E748, 2^31 loaded
 * in DR4), is then the board's: before, it came out a denormal, the
 * quotients 0 or 1, and PHARRIER's sound data took 15 s to unpack, a
 * kilobyte a frame */
#define FR(n_)	ctx->regs.f.f[(n_) ^ 1]
#define DR(n_)	ctx->regs.f.d[(n_)/2]

#define XF(n_)	ctx->regs.x.f[(n_) ^ 1]
#define XD(n_)	ctx->regs.x.d[(n_)/2]

#define FRN	FR(_RN)
#define FRM	FR(_RM)

#define DRN	DR(_RN)
#define DRM	DR(_RM)

#define XDN	XD(_RN)
#define XDM	XD(_RM)

#define IS_PRIVILEGED \
	(SR.bit.md == 1)

#define IS_FP_ENABLED \
	(SR.bit.fd == 0)

#define CHECK_PM \
	do { \
		HL_CPU_ASSERT (ctx, IS_PRIVILEGED); \
	} while (0);

#define CHECK_FP \
	do { \
		HL_CPU_ASSERT (ctx, IS_FP_ENABLED); \
	} while (0);

/* Taken from MAME */
#define SHRINK(addr_) \
	((((addr_) >> 8) & 0xFF00) | ((addr_) & 0xFF))

#define IREG_GET(size_, addr_) \
	ctx->iregs->get (size_, SHRINK (addr_))

#define IREG_PUT(size_, addr_, val_) \
	ctx->iregs->put (size_, SHRINK (addr_), val_)

/* Forward declarations */
static int	sh4_get (Sh4 *, unsigned, uint32_t, void *);
static int	sh4_put (Sh4 *, unsigned, uint32_t, uint64_t);
static void	sh4_update_irqs (Sh4 *ctx);
static void	sh4_mmucr_put (Sh4 *ctx, uint32_t val);

/* Generic Helpers */

static void
swap_r_banks (Sh4 *ctx)
{
	unsigned i;
	for (i = 0; i < 8; i++) {
		uint32_t tmp = R(i);
		R(i) = RBANK(i);
		RBANK(i) = tmp;
	}
}

static void
swap_f_banks (Sh4 *ctx)
{
	unsigned i;
	for (i = 0; i < 16; i++) {
		uint32_t tmp = FR(i).u;
		FR(i).u = XF(i).u;
		XF(i).u = tmp;
	}
}

static unsigned
get_r_bank_num (sh4_sr_t sr)
{
	/* In user mode, bank 0 is always selected; in privileged mode, bank
	 * SR.bit.rb is selected; it follows that bank 1 is only selected if
	 * both MD and RB are set. */
	return sr.bit.md & sr.bit.rb;
}

static void
set_sr (Sh4 *ctx, uint32_t data)
{
	sh4_sr_t old = SR;

	SR.full = data & 0x700083F3;

	/* Swap R banks if required */
	if (get_r_bank_num (old) != get_r_bank_num (SR))
		swap_r_banks (ctx);

	/* If any IRQ-related bits changed, re-validate pending IRQs */
	if ((old.bit.i != SR.bit.i) ||
	    (old.bit.bl != SR.bit.bl))
		sh4_update_irqs (ctx);
}

static uint32_t
get_sr (Sh4 *ctx)
{
	return SR.full;
}

enum {
	SCFSR_DR	= 1,
	SCFSR_RDF	= 2
};

static bool
scif_rx_triggered (Sh4 *ctx)
{
	static const unsigned levels[4] = { 1, 4, 8, 14 };

	return ctx->scif_rx_count >= levels[(IREG_GET (2, SCIF_SCFCR2) >> 6) & 3];
}

static void
scif_status_set (Sh4 *ctx, uint16_t bit)
{
	ctx->scif_status |= bit;
	ctx->scif_status_read &= ~bit;
}

/* The bytes arrive at the speed the SCSMR2 and the SCBRR2 set, not all at once:
 * a game empties the FIFO into a ring of its own, which a long answer would
 * overflow. Each byte's time brings one; RDF tells the FIFO reached its trigger
 * level and, with none left to come, DR that some wait under it: code polling
 * those bits leaves the bytes of a message on their way alone. A byte the FIFO
 * has no room for waits in the pipe. */
static void
scif_receive (Sh4 *ctx, unsigned cycles, bool deliver)
{
	SerialPipe *pipe = ctx->scif_pipe;
	uint16_t smr = IREG_GET (2, SCIF_SCSMR2);
	uint32_t brr = IREG_GET (1, SCIF_SCBRR2);
	/* The start bit, the data, the parity, the stop bits */
	uint32_t bits = 1 + ((smr & 0x40) ? 7 : 8) + ((smr & 0x20) ? 1 : 0) + ((smr & 0x08) ? 2 : 1);
	/* Pphi is 50 MHz on these boards */
	uint32_t bauds = 50000000 / (32 << (2 * (smr & 3))) / (brr + 1);
	uint64_t byte = (uint64_t) ctx->cpu_hz * bits;

	if (!pipe->available () && !ctx->scif_rx_count) {
		ctx->scif_rx_credit = 0;
		return;
	}
	ctx->scif_rx_credit += (uint64_t) cycles * bauds;
	/* Time goes by, up to a FIFO's worth, but the bytes wait for the CPU */
	ctx->scif_rx_credit = MIN2 (ctx->scif_rx_credit, byte * sizeof (ctx->scif_rx));
	if (!deliver)
		return;
	while (ctx->scif_rx_credit >= byte) {
		if (!pipe->available ()) {
			scif_status_set (ctx, SCFSR_DR);
		} else if (ctx->scif_rx_count < sizeof (ctx->scif_rx)) {
			ctx->scif_rx[ctx->scif_rx_count++] = pipe->read ();
			if (scif_rx_triggered (ctx))
				scif_status_set (ctx, SCFSR_RDF);
		} else {
			/* Full */
			ctx->scif_rx_credit = byte;
			break;
		}
		ctx->scif_rx_credit -= byte;
	}
}

void
Sh4::scif_update (unsigned cycles)
{
	Sh4 *ctx = this;
	uint16_t scr;

	if (!scif_pipe)
		return;
	scr = IREG_GET (2, SCIF_SCSCR2);
	/* RE set, and the FIFO not held in reset. The time of this CPU goes by
	 * in slices and its timers' interrupts come at their ends: with RIE,
	 * a byte comes when its interrupt can be taken at once, or a handler
	 * polling the status there would find the bits meant for another one */
	if ((scr & 0x10) && !(IREG_GET (2, SCIF_SCFCR2) & 2)) {
		bool can_take = !irq_ready && intc.irqs[SH4_IESOURCE_RXIF].priority > SR.bit.i;

		scif_receive (this, cycles, !(scr & 0x40) || can_take);
	}
	if (scr & 0x80)
		set_irq_state (SH4_IESOURCE_TXIF, hl::IRQ_STATE_RAISED);
	if ((scr & 0x40) && scif_status)
		set_irq_state (SH4_IESOURCE_RXIF, hl::IRQ_STATE_RAISED);
}

static void
set_fpscr (Sh4 *ctx, uint32_t val)
{
	sh4_fpscr_t old = FPSCR;

	FPSCR.full = val & 0x003FFFFF;
	/* Swap FR banks if required */
	if (old.bit.fr != FPSCR.bit.fr)
		swap_f_banks (ctx);

	/* SZ and PR both set is only undefined for the FPU's arithmetic: the
	 * moves do as SZ says, and Strike Fighter's kernel sets SZ to save the
	 * registers of a task it interrupted in double precision */
}

static uint32_t
get_fpscr (Sh4 *ctx)
{
	return FPSCR.full;
}

/* Port A */

/* XXX Port A emulation is still very rough; we probably want to notify the
 * external handlers about the directions of all bits; although they already
 * probably know what to do with them. */

static void
set_porta (Sh4 *ctx, uint16_t data)
{
	uint32_t pctra;
	uint16_t pdtra;
	unsigned i;

	HL_ASSERT (ctx->porta.put);

	pdtra = IREG_GET (2, BSC_PDTRA);
	pctra = IREG_GET (4, BSC_PCTRA);

	for (i = 0; i < 16; i++) {
		unsigned direction = (pctra >> (i * 2)) & 1;
		/* only override those bits that are set to output */
		if (direction != 0) {
			pdtra &= ~(1 << i);
			pdtra |= data & (1 << i);
		}
	}
	if (ctx->porta.put (ctx, pdtra))
		HL_ASSERT (0);
}

static uint16_t
get_porta (Sh4 *ctx)
{
	uint32_t pctra;
	uint16_t pdtra, data;
	unsigned i;

	HL_ASSERT (ctx->porta.get);

	pdtra = IREG_GET (2, BSC_PDTRA);
	pctra = IREG_GET (4, BSC_PCTRA);

	if (ctx->porta.get (ctx, &data))
		HL_ASSERT (0);
	for (i = 0; i < 16; i++) {
		unsigned direction = (pctra >> (i * 2)) & 1;
		/* only override those bits that are set to input */
		if (direction == 0) {
			pdtra &= ~(1 << i);
			pdtra |= data & (1 << i);
		}
	}
	return pdtra;
}

/* Interrupt Controller */

static void
set_irq_priority (Sh4 *ctx, unsigned num, unsigned priority)
{
	/* An IRQ priority can't be lowered while an IRQ is firing */
	HL_ASSERT ((ctx->intc.irqs[num].state != hl::IRQ_STATE_RAISED) ||
	           (ctx->intc.irqs[num].priority <= priority));
	ctx->intc.irqs[num].priority = priority;
}

static void
sh4_intc_update_priorities (Sh4 *ctx)
{
	uint16_t ipra = IREG_GET (2, INTC_IPRA);
	uint16_t iprb = IREG_GET (2, INTC_IPRB);
	uint16_t iprc = IREG_GET (2, INTC_IPRC);

	/* See Table 19.5, "Interrupt Exception Sources and Priority Order" */
	set_irq_priority (ctx, SH4_IESOURCE_UDI, iprc & 15);
	set_irq_priority (ctx, SH4_IESOURCE_GPIOI, (iprc >> 12) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_DMTE0, (iprc >> 8) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_DMTE1, (iprc >> 8) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_DMTE2, (iprc >> 8) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_DMTE3, (iprc >> 8) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_DMAE, (iprc >> 8) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_TUNI0, (ipra >> 12) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_TUNI1, (ipra >> 8) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_TUNI2, (ipra >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_TICPI2, (ipra >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_ATI, ipra & 15);
	set_irq_priority (ctx, SH4_IESOURCE_PRI, ipra & 15);
	set_irq_priority (ctx, SH4_IESOURCE_CUI, ipra & 15);
	set_irq_priority (ctx, SH4_IESOURCE_ERI, (iprb >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_RXI, (iprb >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_TXI, (iprb >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_TEI, (iprb >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_ERIF, (iprc >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_RXIF, (iprc >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_BRIF, (iprc >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_TXIF, (iprc >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_ITI, (iprb >> 12) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_RCMI, (iprb >> 8) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_ROVI, (iprb >> 8) & 15);

	/* Update the pending flag for the new priorities */
	sh4_update_irqs (ctx);
}

/* DMA Controller */

/* XXX DTR mode */
/* XXX validate RS settings against SAR and DAR */
/* XXX raise a DMA AE if any access error occurs. */
/* XXX synchronization with the TMU if required */
/* XXX most of this stuff can be set at write time */

static const uint32_t ts_incr[8] = { 8, 1, 2, 4, 32, 0, 0, 0 };

#define DMAC_DO_TRANSFER(size_) \
	do { \
		for (; cycles > 0 && tcr > 0; cycles--, tcr--) { \
			uint64_t tmp; \
			if (sm == 2) \
				sar -= size_; \
			if (dm == 2) \
				dar -= size_; \
			if ((size_) == 32) { \
				sh4_get (ctx, 8, sar, &tmp); \
				sh4_put (ctx, 8, dar, tmp); \
				sh4_get (ctx, 8, sar+8, &tmp); \
				sh4_put (ctx, 8, dar+8, tmp); \
				sh4_get (ctx, 8, sar+16, &tmp); \
				sh4_put (ctx, 8, dar+16, tmp); \
				sh4_get (ctx, 8, sar+24, &tmp); \
				sh4_put (ctx, 8, dar+24, tmp); \
			} else { \
				sh4_get (ctx, size_, sar, &tmp); \
				sh4_put (ctx, size_, dar, tmp); \
			} \
			if (sm == 1) \
				sar += size_; \
			if (dm == 1) \
				dar += size_; \
		} \
	} while (0)

static void
sh4_dmac_run_channel (Sh4 *ctx, unsigned ch, int cycles)
{
	uint32_t offs = ch * 0x10;
	uint32_t sar  = IREG_GET (4, DMAC_SAR0 + offs);
	uint32_t dar  = IREG_GET (4, DMAC_DAR0 + offs);
	uint32_t tcr  = IREG_GET (4, DMAC_TCR0 + offs);
	uint32_t chcr = IREG_GET (4, DMAC_CHCR0 + offs);

	uint32_t ts = (chcr >> 4) & 7;
	uint32_t sm = (chcr >> 12) & 3;
	uint32_t dm = (chcr >> 14) & 3;

	HL_CPU_LOG (ctx, "DMAC ch%u: %08X->%08X x %X [%uB, sm=%u, dm=%u]",
	            ch, sar, dar, tcr, ts_incr[ts], sm, dm);

	switch (ts) {
	case 0: /* 8 bytes */
		DMAC_DO_TRANSFER (8);
		break;
	case 1: /* 1 byte */
		DMAC_DO_TRANSFER (1);
		break;
	case 2: /* 2 bytes */
		DMAC_DO_TRANSFER (2);
		break;
	case 3: /* 4 bytes */
		DMAC_DO_TRANSFER (4);
		break;
	case 4: /* 32 bytes */
		DMAC_DO_TRANSFER (32);
		break;
	default:
		HL_ASSERT (!"invalid DMAC transfer size");
		break;
	}

	IREG_PUT (4, DMAC_SAR0 + offs, sar);
	IREG_PUT (4, DMAC_DAR0 + offs, dar);
	IREG_PUT (4, DMAC_TCR0 + offs, tcr);

	if (tcr == 0) {
		chcr |= 2; /* TE */
		if (chcr & 4) { /* IE */
			unsigned num;
			num = (ch == 0) ? SH4_IESOURCE_DMTE0 :
			      (ch == 1) ? SH4_IESOURCE_DMTE1 :
			      (ch == 2) ? SH4_IESOURCE_DMTE2 :
			      SH4_IESOURCE_DMTE3;
			ctx->set_irq_state (num, hl::IRQ_STATE_RAISED);
		}
		ctx->dmac.is_running[ch] = false;
		IREG_PUT (4, DMAC_CHCR0 + offs, chcr);
	}
}

static void
sh4_dmac_run (Sh4 *ctx, int cycles)
{
	/* TODO: priorities (DMAOR.PR). Are they really that important? */

	if (ctx->dmac.is_running[0])
		sh4_dmac_run_channel (ctx, 0, cycles);
	if (ctx->dmac.is_running[1])
		sh4_dmac_run_channel (ctx, 1, cycles);
	if (ctx->dmac.is_running[2])
		sh4_dmac_run_channel (ctx, 2, cycles);
	if (ctx->dmac.is_running[3])
		sh4_dmac_run_channel (ctx, 3, cycles);
}

static void
sh4_dmac_update_channel_state (Sh4 *ctx, unsigned ch, uint32_t request_type)
{
	uint32_t offs = ch * 0x10;
	uint32_t dmaor = IREG_GET (4, DMAC_DMAOR);
	uint32_t chcr = IREG_GET (4, DMAC_CHCR0 + offs);

	HL_ASSERT (ch < 4);
	ctx->dmac.is_running[ch] = false;

	/* Check that both DME and DE are set */
	if (dmaor & chcr & 1) {
		uint32_t sar = IREG_GET (4, DMAC_SAR0 + offs);
		uint32_t dar = IREG_GET (4, DMAC_DAR0 + offs);
		uint32_t ts = (chcr >> 4) & 7;
		uint32_t rs = (chcr >> 8) & 15;
		uint32_t sm = (chcr >> 12) & 3;
		uint32_t dm = (chcr >> 14) & 3;

		HL_ASSERT (ts < 5);
		HL_ASSERT (sm != 3);
		HL_ASSERT (dm != 3);
		HL_ASSERT (rs != 1 && rs != 7 && rs != 15);
//		HL_ASSERT (AREA (dar) != 7);

		/* Check the addresses and update AE if needed; bail out and
		 * send an Address Error exception. We only do it here,
		 * because tick_channel () can't alter the addresses as to
		 * raise an AE if they are correct here (by induction.) */
		if ((sar | dar) & (ts_incr[ts] - 1)) {
			HL_CPU_LOG (ctx, "DMAC: raising DMA address error");
			ctx->set_irq_state (SH4_IESOURCE_DMAE, hl::IRQ_STATE_RAISED);
			return;
		}

		/* Check if NMIF, AE or TE have been set */
		if ((dmaor & 6) || (chcr & 2))
			return;

		/* All checks passed; this DMA channel may now run */
		if ((rs >> 2) == request_type) {
			HL_CPU_LOG (ctx, "DMAC: enabling channel %u", ch);
			ctx->dmac.is_running[ch] = true;
		}
	}
}

static void
sh4_dmac_update_state (Sh4 *ctx, uint32_t request_type)
{
	sh4_dmac_update_channel_state (ctx, 0, request_type);
	sh4_dmac_update_channel_state (ctx, 1, request_type);
	sh4_dmac_update_channel_state (ctx, 2, request_type);
	sh4_dmac_update_channel_state (ctx, 3, request_type);
}

/* An external device's request, one 32-bit word (the Gaelco boards' link): the
 * channel set up for it, with its count left */
bool
Sh4::dma_ready (unsigned ch)
{
	Sh4 *ctx = this;
	uint32_t offs = ch * 0x10;
	uint32_t chcr = IREG_GET (4, DMAC_CHCR0 + offs);
	uint32_t rs = (chcr >> 8) & 15;

	return (IREG_GET (4, DMAC_DMAOR) & chcr & 1) && !(IREG_GET (4, DMAC_DMAOR) & 6) && !(chcr & 2) &&
	       (rs == 2 || rs == 3) && ((chcr >> 4) & 7) == 3 && IREG_GET (4, DMAC_TCR0 + offs) != 0;
}

static void
sh4_dma_word_done (Sh4 *ctx, unsigned ch)
{
	uint32_t offs = ch * 0x10;
	uint32_t tcr = IREG_GET (4, DMAC_TCR0 + offs) - 1;

	IREG_PUT (4, DMAC_TCR0 + offs, tcr);
	if (!tcr) {
		uint32_t chcr = IREG_GET (4, DMAC_CHCR0 + offs);
		IREG_PUT (4, DMAC_CHCR0 + offs, chcr | 2);
		if (chcr & 4)
			ctx->set_irq_state (ch == 0 ? SH4_IESOURCE_DMTE0 : ch == 1 ? SH4_IESOURCE_DMTE1 :
			                    ch == 2 ? SH4_IESOURCE_DMTE2 : SH4_IESOURCE_DMTE3,
			                    hl::IRQ_STATE_RAISED);
	}
}

void
Sh4::dma_to_memory (unsigned ch, uint32_t word)
{
	Sh4 *ctx = this;
	uint32_t offs = ch * 0x10;
	uint32_t dar = IREG_GET (4, DMAC_DAR0 + offs);
	unsigned dm = (IREG_GET (4, DMAC_CHCR0 + offs) >> 14) & 3;

	if (dm == 2)
		dar -= 4;
	sh4_put (this, 4, dar, word);
	if (dm == 1)
		dar += 4;
	IREG_PUT (4, DMAC_DAR0 + offs, dar);
	sh4_dma_word_done (this, ch);
}

uint32_t
Sh4::dma_from_memory (unsigned ch)
{
	Sh4 *ctx = this;
	uint32_t offs = ch * 0x10;
	uint32_t sar = IREG_GET (4, DMAC_SAR0 + offs);
	unsigned sm = (IREG_GET (4, DMAC_CHCR0 + offs) >> 12) & 3;
	uint64_t word = 0;

	if (sm == 2)
		sar -= 4;
	sh4_get (this, 4, sar, &word);
	if (sm == 1)
		sar += 4;
	IREG_PUT (4, DMAC_SAR0 + offs, sar);
	sh4_dma_word_done (this, ch);
	return (uint32_t) word;
}

/* Timer Unit */

/* Note: for performance reasons, TCNT{0,1,2} are handled differently than
 * the other on-chip registers, and are defined as uint32_t directly. */

#define TMU_TCOR(n_)	(TMU_TCOR0 + (n_) * 12)
#define TMU_TCNT(n_)	(TMU_TCNT0 + (n_) * 12)
#define TMU_TCR(n_)	(TMU_TCR0 + (n_) * 12)

/* A channel's ticks a second, times 8: it counts Pφ (pclk_hz: the board's own)
 * / 4, 16, 64, 256 or 1024 (TCR's TPSC); the RTC's 16384 Hz with 6; TCLK with
 * 7, taken as Pφ / 4. Against the CPU's own clock (cpu_hz), so that they keep
 * real time whatever it is */
static uint64_t
sh4_tmu_rate8 (Sh4 *ctx, unsigned ch)
{
	unsigned tpsc = IREG_GET (2, TMU_TCR (ch)) & 7;

	if (tpsc <= 4)
		return (uint64_t) (ctx->pclk_hz * 2) >> (2 * tpsc);
	if (tpsc == 6)
		return 8 * 16384;
	return (uint64_t) ctx->pclk_hz * 2;
}

static void
sh4_tmu_run_channel (Sh4 *ctx, unsigned ch, int cycles)
{
	uint32_t counter = ctx->tmu.counter[ch];
	uint64_t per = 8ull * ctx->cpu_hz;
	uint64_t total = ctx->tmu.cycles[ch] + (uint64_t) cycles * sh4_tmu_rate8 (ctx, ch);

	/* The ticks, from the CPU's cycles */
	cycles = (int) (total / per);
	ctx->tmu.cycles[ch] = total % per;

	/* The cycles up to the next underflow at once, as many decrements
	 * of a non-zero counter as there are */
	while (cycles > 0) {
		if (counter != 0) {
			uint32_t d = MIN2 ((uint32_t) cycles, counter);
			counter -= d;
			cycles -= (int) d;
			continue;
		}
		/* Underflow */
		{
			uint16_t tcr;

			/* Set UNF */
			tcr = IREG_GET (2, TMU_TCR (ch));
			IREG_PUT (2, TMU_TCR (ch), tcr | 0x100);

			/* Reload the timer */
			counter = IREG_GET (4, TMU_TCOR (ch));

			/* Raise an IRQ if UNIE is set */
			if (tcr & 0x20) {
				static const unsigned nums[3] = {
					SH4_IESOURCE_TUNI0,
					SH4_IESOURCE_TUNI1,
					SH4_IESOURCE_TUNI2,
				};

				HL_CPU_LOG (ctx, "TMU: rising ch%u IRQ", ch);

				ctx->set_irq_state (nums[ch], hl::IRQ_STATE_RAISED);
			}
		}
		/* The underflow's cycle counts down from the reload */
		counter--;
		cycles--;
	}

	ctx->tmu.counter[ch] = counter;
}

/* A TCNT read. A loop polling the timer (the games' delays and timed waits,
 * PHARRIER's for a raster line, PH:@0C01C14A) reads it from the same place
 * again (the same PC, PR and r15), storing nothing but to its stack in
 * between: the rest of the slice is waited at once, as the idle loops'
 * (the timers count the whole slice anyway). One read a slice then, not
 * thousands; off where the idle loops are. A wait that keeps the time it
 * read in memory stores away from the stack at each turn (ATV Track's
 * master waits for its frame so, two words at each turn, AT:@0C075186): as
 * many stores as in the turn before, the GPRs as they were at the read
 * before (a copy timed so moves its pointers: not waited) */
static void
sh4_tmu_polled (Sh4 *ctx)
{
	hl::Cpu *cpu = ctx;
	bool place = ctx->poll.pc == PC && ctx->poll.pr == ctx->regs.pr &&
	             ctx->poll.sp == ctx->regs.r[15];
	uint64_t turn = ctx->far_stores - ctx->poll.far_stores;
	bool again = place &&
	             (turn == 0 || (turn == ctx->poll.far_turn &&
	                            !memcmp (ctx->poll.r, ctx->regs.r, sizeof (ctx->poll.r))));

	ctx->poll.pc = PC;
	ctx->poll.pr = ctx->regs.pr;
	ctx->poll.sp = ctx->regs.r[15];
	ctx->poll.far_stores = ctx->far_stores;
	ctx->poll.far_turn = place ? turn : ~(uint64_t) 0;
	memcpy (ctx->poll.r, ctx->regs.r, sizeof (ctx->poll.r));
	if (again && ctx->idle.enabled && !cpu->profile && cpu->breakpoints.empty () &&
	    cpu->watchpoints.empty () && cpu->remaining > 1) {
		ctx->cycles_skipped += (uint64_t) (cpu->remaining - 1);
		cpu->remaining = 1;
	}
}

static void
sh4_tmu_run (Sh4 *ctx, int cycles)
{
	if (ctx->tmu.is_running[0])
		sh4_tmu_run_channel (ctx, 0, cycles);
	if (ctx->tmu.is_running[1])
		sh4_tmu_run_channel (ctx, 1, cycles);
	if (ctx->tmu.is_running[2])
		sh4_tmu_run_channel (ctx, 2, cycles);
}

static void
sh4_tmu_update_freq (Sh4 *ctx)
{
	/* TODO */
}

static void
sh4_tmu_update_state (Sh4 *ctx)
{
	uint8_t tstr = IREG_GET (1, TMU_TSTR);

	ctx->tmu.is_running[0] = tstr & 1;
	ctx->tmu.is_running[1] = (tstr >> 1) & 1;
	ctx->tmu.is_running[2] = (tstr >> 2) & 1;

	HL_CPU_LOG (ctx, "TMU: settings states: %u, %u, %u",
	            ctx->tmu.is_running[0],
	            ctx->tmu.is_running[1],
	            ctx->tmu.is_running[2]);
}

/* On-chip Modules
 * See Table A.1, "Address List" */

static int
sh4_ireg_get (Sh4 *ctx, unsigned size, uint32_t addr, void *val)
{
	HL_CPU_LOG (ctx, "IREG R%u %08X", size * 8, addr);

	set_ptr (val, size, IREG_GET (size, addr));

	switch (addr & 0xFFFFFF) {
	case 0x000030:
		/* The processor's version (SH7091), which the NAOMI's BIOS and
		 * games read */
		set_ptr (val, size, 0x040205C1);
		return 0;
	case RTC_R64CNT:
		set_ptr (val, size, ctx->rfcr++ & 0x7F);
		return 0;
	case BSC_RFCR:
		if (ctx->rfcr_counts) {
			/* The refreshes counted, which the NAOMI's BIOS waits for */
			set_ptr (val, size, 0x11 + (ctx->rfcr++ & 0x1FF));
			break;
		}
		/* fall through */
	case BSC_PDTRA:
	case CPG_WTCSR:
		HL_ASSERT (size == 2);
		set_ptr (val, size, get_porta (ctx));
		break;
	case CCN_CCR:
	case CCN_INTEVT:
	case BSC_PCTRA:
		HL_ASSERT (size == 4);
		break;
	/* The bus state controller's: what was written */
	case BSC_BCR1:
	case BSC_BCR2:
	case BSC_WCR1:
	case BSC_WCR2:
	case BSC_WCR3:
	case BSC_MCR:
	case BSC_PCR:
	case BSC_RTCSR:
	case BSC_RTCNT:
	case BSC_RTCOR:
		break;
	/* The MMU's, the exceptions' */
	case CCN_PTEH:
	case CCN_PTEL:
	case CCN_TTB:
	case CCN_TEA:
	case CCN_PTEA:
	case CCN_TRA:
	case CCN_EXPEVT:
		break;
	case CCN_MMUCR:
		set_ptr (val, size, (IREG_GET (4, CCN_MMUCR) & ~0xFC00) | (ctx->mmu.urc << 10));
		break;
	/* INTC */
	case INTC_IPRA:
	case INTC_IPRB:
	case INTC_IPRC:
		set_ptr (val, size, IREG_GET (2, addr));
		break;
	/* Port B, the GPIO interrupts' control: what was written (ATV
	 * Track's) */
	case BSC_PCTRB:
	case BSC_PDTRB:
	case BSC_GPIOIC:
		break;
	/* TMU */
	case TMU_TSTR:
		HL_ASSERT (size == 1);
		break;
	case TMU_TCNT0:
		HL_ASSERT (size == 4);
		set_ptr (val, size, ctx->tmu.counter[0]);
		sh4_tmu_polled (ctx);
		break;
	case TMU_TCNT1:
		set_ptr (val, size, ctx->tmu.counter[1]);
		break;
	case TMU_TCNT2:
		set_ptr (val, size, ctx->tmu.counter[2]);
		break;
	/* What was written (UNF set by the timers): ATV Track's interrupt
	 * handler reads TCR1 and TCR2 */
	case TMU_TOCR:
	case TMU_TCOR0:
	case TMU_TCOR1:
	case TMU_TCOR2:
	case TMU_TCR0:
	case TMU_TCR1:
	case TMU_TCR2:
	case TMU_TCPR2:
	case CCN_QACR0:
	case CCN_QACR1:
		break;
	/* The SCIF: always ready to send, nothing received */
	case SCIF_SCFSR2:
		/* A pipe on it: no error flag, whatever was written back */
		if (ctx->scif_pipe)
		{
			ctx->scif_status_read = ctx->scif_status;
			set_ptr (val, size, 0x0060 | ctx->scif_status);
		}
		else
			set_ptr (val, size, (IREG_GET (2, addr) & ~0x0003) | 0x0060);
		break;
	case SCIF_SCFDR2:
		set_ptr (val, size, ctx->scif_pipe ? ctx->scif_rx_count : 0);
		break;
	case SCIF_SCFRDR2:
		if (ctx->scif_rx_count) {
			set_ptr (val, size, ctx->scif_rx[0]);
			memmove (ctx->scif_rx, ctx->scif_rx + 1, --ctx->scif_rx_count);
		} else {
			set_ptr (val, size, 0);
		}
		break;
	case SCIF_SCLSR2:
		if (ctx->scif_pipe) {
			set_ptr (val, size, 0);
			return 0;
		}
		break;
	case SCIF_SCSMR2:
	case SCIF_SCBRR2:
	case SCIF_SCSCR2:
	case SCIF_SCFCR2:
	case SCIF_SCSPTR2:
		break;
	/* The SCI, likewise: TDRE and TEND set, nothing received (gfootbal
	 * waits for TDRE) */
	case SCI_SCSSR1:
		set_ptr (val, size, (IREG_GET (1, addr) & 0x03) | 0x84);
		break;
	case SCI_SCRDR1:
		set_ptr (val, size, 0);
		break;
	case SCI_SCSMR1:
	case SCI_SCBRR1:
	case SCI_SCSCR1:
	case SCI_SCTDR1:
	case SCI_SCSCMR1:
	case SCI_SCSPTR1:
		break;
	default:
		/* DMAC */
		if (IN_RANGE (addr & 0xFFFFFF, DMAC_SAR0, DMAC_DMAOR)) {
			HL_ASSERT (size == 4);
			break;
		}
		/* Invalid/Unhandled */
		return -1;
	}
	return 0;
}

/* TODO: mask writes to read-only bits */

/* Writes to SAR0,DAR0,TCR0,CHCR0 are masked when DMAOR.DDT is set */
#define DMAC_MASK_ON_DDT \
	{ \
		uint32_t dmaor = IREG_GET (4, DMAC_DMAOR); \
		if ((dmaor & 0x8000) && (ch == 0)) \
			return 0; \
	}

static int
sh4_ireg_put (Sh4 *ctx, unsigned size, uint32_t addr, uint64_t val)
{
	HL_CPU_LOG (ctx, "IREG W%u %08X = %lX", size * 8, addr, val);

	switch (addr & 0xFFFFFF) {
	case CPG_STBCR:
		HL_ASSERT (size == 1);
		break;
	case BSC_BCR2:
	case BSC_PCR:
	case BSC_RTCSR:
	case BSC_RTCNT:
	case BSC_RTCOR:
	case BSC_RFCR:
	case CPG_WTCSR:
		HL_ASSERT (size == 2);
		break;
	case BSC_PDTRA:
		HL_ASSERT (size == 2);
		set_porta (ctx, val);
		break;
	case BSC_PCTRB:
	case BSC_PDTRB:
	case BSC_GPIOIC:
		break;
	case CCN_MMUCR:
		HL_ASSERT (size == 4);
		sh4_mmucr_put (ctx, (uint32_t) val);
		return 0;
	/* The SCIF: what is sent goes to the log, a line at a time (ATV
	 * Track's); the SCI's too */
	case SCI_SCTDR1:
	case SCIF_SCFTDR2:
		if ((addr & 0xFFFFFF) == SCIF_SCFTDR2 && ctx->scif_pipe) {
			ctx->scif_pipe->write ((uint8_t) val);
			return 0;
		}
		if (val == '\n' || ctx->scif_line.size () >= 200) {
			HL_PRINT ("%s: SCIF: %s", ctx->name ? ctx->name : "sh4", ctx->scif_line.c_str ());
			ctx->scif_line.clear ();
		} else if (val != '\r')
			ctx->scif_line += (char) val;
		return 0;
	case SCIF_SCFCR2:
		/* RFRST */
		if (val & 2)
			ctx->scif_rx_count = 0;
		break;
	case SCIF_SCFSR2:
		if (ctx->scif_pipe) {
			uint16_t keep = (uint16_t) val | ~ctx->scif_status_read;

			/* Those bits stay as long as what sets them is there */
			if (scif_rx_triggered (ctx))
				keep |= SCFSR_RDF;
			if (ctx->scif_rx_count)
				keep |= SCFSR_DR;
			ctx->scif_status &= keep;
			ctx->scif_status_read &= keep;
			/* The interrupt follows the bits at once: the line's next
			 * round would leave them set for others to find */
			ctx->set_irq_state (SH4_IESOURCE_RXIF,
			                    (ctx->scif_status && (IREG_GET (2, SCIF_SCSCR2) & 0x40)) ?
			                    hl::IRQ_STATE_RAISED : hl::IRQ_STATE_CLEAR);
		}
		break;
	case SCIF_SCSMR2:
	case SCIF_SCBRR2:
	case SCIF_SCSCR2:
	case SCIF_SCSPTR2:
	case SCIF_SCLSR2:
	case SCI_SCSMR1:
	case SCI_SCBRR1:
	case SCI_SCSCR1:
	case SCI_SCSSR1:
	case SCI_SCSCMR1:
	case SCI_SCSPTR1:
	case CCN_INTEVT:
		break;
	case CCN_PTEH:
	case CCN_PTEL:
	case CCN_TTB:
	case CCN_TEA:
	case CCN_PTEA:
	case CCN_TRA:
	case CCN_EXPEVT:
	case CCN_QACR0:
	case CCN_QACR1:
	case BSC_BCR1:
	case BSC_WCR1:
	case BSC_WCR2:
	case BSC_WCR3:
	case BSC_MCR:
	case BSC_PCTRA:
		/* The NAOMI's BIOS writes them in 16 bits too */
		HL_ASSERT (size == 4 || ctx->rfcr_counts);
		break;
	case CCN_CCR:
		HL_ASSERT (size == 4 || ctx->rfcr_counts);
		/* ICI empties the instruction cache, read back as 0 as OCI;
		 * ICE switched, its lines not kept */
		if ((val & 0x800) || ((val ^ IREG_GET (4, CCN_CCR)) & 0x100)) {
			ctx->blocks.icache.reset ();
			ctx->blocks.icache_changed ();
		}
		val &= ~(uint64_t) 0x808;
		break;
	/* UBC */
	case UBC_BBRA:
	case UBC_BBRB:
		HL_ASSERT (size == 2);
		break;
	/* INTC */
	case INTC_ICR:
		{
			uint16_t old = IREG_GET (2, addr);
			HL_ASSERT (size == 2);
			/* The reserved bits are not kept (gfootbal sets some) */
			if (val & ~0xC380) {
				static bool told = false;
				if (!told)
					HL_CPU_ERROR (ctx, "ICR = %04X: reserved bits", (unsigned) val);
				told = true;
			}
			/* ICR.NMIL is read only */
			IREG_PUT (2, addr, (old & 0x8000) | (val & 0x4380));
			sh4_update_irqs (ctx);
		}
		return 0;
	/* 16 bits; ATV Track writes them 32 bits at a time */
	case INTC_IPRA:
	case INTC_IPRC:
		IREG_PUT (2, addr, val & 0xFFFF);
		sh4_intc_update_priorities (ctx);
		return 0;
	case INTC_IPRB:
		/* Bits 3-0 are reserved */
		IREG_PUT (2, addr, val & 0xFFF0);
		sh4_intc_update_priorities (ctx);
		return 0;
	/* DMAC */
	case DMAC_SAR0:
	case DMAC_SAR1:
	case DMAC_SAR2:
	case DMAC_SAR3:
	case DMAC_DAR0:
	case DMAC_DAR1:
	case DMAC_DAR2:
	case DMAC_DAR3:
		{
			unsigned ch = (addr >> 4) & 3;
			DMAC_MASK_ON_DDT;
			HL_ASSERT (size == 4);
			HL_ASSERT (!(ctx->dmac.is_running[ch]));
		}
		break;
	case DMAC_TCR0:
	case DMAC_TCR1:
	case DMAC_TCR2:
	case DMAC_TCR3:
		{
			unsigned ch = (addr >> 4) & 3;
			DMAC_MASK_ON_DDT;
			HL_ASSERT (size == 4);
			/* violated by SGNASCAR @0C071EA8:
			HL_ASSERT (!(val & 0xFF000000));
			*/
			HL_ASSERT (!(ctx->dmac.is_running[ch]));
		}
		break;
	case DMAC_CHCR0:
	case DMAC_CHCR1:
	case DMAC_CHCR2:
	case DMAC_CHCR3:
		{
			unsigned ch = (addr >> 4) & 3;
			uint32_t old = IREG_GET (size, addr);
			DMAC_MASK_ON_DDT;
			HL_ASSERT (size == 4);
			HL_ASSERT (!(val & 0x00F00008));
			HL_ASSERT ((ch < 2) || !(val & 0x00050000));
			/* Make sure that TE doesn't get set */
			IREG_PUT (size, addr, (val & ~2) | (old & val & 2));
			sh4_dmac_update_channel_state (ctx, ch, 1);
			sh4_dmac_run (ctx, 0x7FFFFFFF);
		}
		return 0;
	case DMAC_DMAOR:
		{
			uint32_t old = IREG_GET (size, addr);
			uint32_t nmil = IREG_GET (2, INTC_ICR) & 0x8000 ? 1 : 0;
			HL_ASSERT (size == 4);
			HL_ASSERT (!(val & 0xFFFF7CF8));
			/* DDT is unsupported */
			//HL_ASSERT (!(val & 0x8000)); /* XXX used in Hikaru... */
			/* Make sure that AE and NMIF don't get set; also
			 * make sure that NMIF is never cleared if an NMI
			 * is still raised. */
			IREG_PUT (size, addr, (val & ~6) | (old & val & 6) | (nmil << 1));
			sh4_dmac_update_state (ctx, 1);
			sh4_dmac_run (ctx, 0x7FFFFFFF);
		}
		return 0;
	/* TMU */
	case TMU_TOCR:
		HL_ASSERT (size == 1);
		HL_ASSERT (!(val & 0xFE));
		IREG_PUT (size, addr, val);
		sh4_tmu_update_freq (ctx);
		return 0;
	case TMU_TSTR:
		HL_ASSERT (size == 1);
		HL_ASSERT (!(val & 0xF8));
		IREG_PUT (size, addr, val);
		sh4_tmu_update_state (ctx);
		return 0;
	case TMU_TCR0:
	case TMU_TCR1:
		{
			uint16_t old = IREG_GET (size, addr);
			HL_ASSERT (size == 2);
			HL_ASSERT (!(val & 0xFEC0));
			/* Make sure not to set UNF */
			IREG_PUT (size, addr, (val & 0x00FF) | (old & val & 0x0100));
			sh4_tmu_update_freq (ctx);
		}
		return 0;
	case TMU_TCR2:
		{
			uint16_t old = IREG_GET (size, addr);
			HL_ASSERT (size == 2);
		 	HL_ASSERT (!(val & 0xFC00));
			/* XXX input capture is unsupported */
			HL_ASSERT (!(val & 0x0080));
			/* Make sure not to set ICPF, UNF */
			IREG_PUT (size, addr, (val & 0x00FF) | (old & val & 0x0300));
			sh4_tmu_update_freq (ctx);
		}
		return 0;
	case TMU_TCOR0:
	case TMU_TCOR1:
	case TMU_TCOR2:
		HL_ASSERT (size == 4);
		break;
	case TMU_TCNT0:
		HL_ASSERT (size == 4);
		ctx->tmu.counter[0] = val;
		return 0;
	case TMU_TCNT1:
		HL_ASSERT (size == 4);
		ctx->tmu.counter[1] = val;
		return 0;
	case TMU_TCNT2:
		HL_ASSERT (size == 4);
		ctx->tmu.counter[2] = val;
		return 0;
	default:
		if (IN_RANGE (addr & 0xFFFFFF, 0x900000, 0x90FFFF) || /* BSC_SDRM2 */
		    IN_RANGE (addr & 0xFFFFFF, 0x940000, 0x94FFFF)) { /* BSC_SDRM3 */
			HL_ASSERT (size == 1);
			break;
		}
		/* Invalid/Unhandled */
		return -1;
	}
	IREG_PUT (size, addr, val);
	return 0;
}

/* MMU
 * ===
 *
 * While MMUCR.AT is set, the CPU's own accesses to U0/P0 (00000000-7FFFFFFF)
 * and P3 (C0000000-DFFFFFFF) go through the TLBs: the UTLB's 64 entries for
 * the data, the ITLB's 4 for the code, which the UTLB fills on a miss as the
 * hardware does. Pages of 1 KB, 4 KB, 64 KB or 1 MB; their ASID must be
 * PTEH's, unless shared (SH), or MMUCR.SV in privileged mode. In user mode,
 * P1, P2 and P4 are address errors (the store queues aside, unless SQMD).
 *
 * A miss, a protection violation or the first write to a clean page (D
 * clear) takes its exception: the instruction is undone (Sh4Fault unwinds
 * it before it changed anything: its pre-decrement comes after its store),
 * TEA and PTEH.VPN get the address, the handler at VBR + 400 (the misses)
 * or VBR + 100 runs, and its RTE runs the instruction again (from the
 * branch, for a delay slot's). LDTLB loads the UTLB entry MMUCR.URC from
 * PTEH, PTEL and PTEA; URC counts the UTLB's lookups, up to URB. The TLBs'
 * arrays are mapped at F2000000-F3FFFFFF and F6000000-F7FFFFFF, the UTLB
 * address array's associative writes too. The DMAC and the debugger see
 * physical addresses.
 *
 * While AT is set the CPU runs in the interpreter alone: no decoded blocks,
 * no translations to x86-64 (sh4_run_blocks ()). The caches are not
 * emulated, nor the TLB multiple hits (the first entry wins). Smashing
 * Drive's.
 */

/* An exception taken: the instruction it came from unwound */
struct Sh4Fault {};

enum {
	EXP_TLB_MISS_R		= 0x040,	/* and the ITLB's */
	EXP_TLB_MISS_W		= 0x060,
	EXP_INITIAL_PAGE_W	= 0x080,
	EXP_TLB_PROT_R		= 0x0A0,	/* and the ITLB's */
	EXP_TLB_PROT_W		= 0x0C0,
	EXP_ADDR_ERR_R		= 0x0E0,	/* and the fetches' */
	EXP_ADDR_ERR_W		= 0x100,
	EXP_TRAPA		= 0x160,
};

/* Enters an exception's handler, VBR + offset: SPC the instruction to run
 * next (or again), SSR and SGR the state before; privileged, the bank 1,
 * blocked */
static void
sh4_enter_exception (Sh4 *ctx, uint32_t expevt, uint32_t offset, uint32_t spc)
{
	sh4_sr_t sr;

	if (SR.bit.bl)
		HL_CPU_ERROR (ctx, "exception %03X while blocked (a reset on the hardware)", expevt);
	SPC = spc;
	SSR = SR;
	SGR = R (15);
	IREG_PUT (4, CCN_EXPEVT, expevt);
	sr.full = SR.full;
	sr.bit.md = 1;
	sr.bit.rb = 1;
	sr.bit.bl = 1;
	set_sr (ctx, sr.full);
	PC = VBR + offset;
	HL_CPU_LOG (ctx, "exception %03X: SPC=%08X TEA=%08X, to %08X",
	            expevt, spc, (uint32_t) IREG_GET (4, CCN_TEA), PC);
}

/* An access's exception: the instruction (a delay slot's branch) run again
 * after the handler */
[[noreturn]] static void
sh4_fault (Sh4 *ctx, uint32_t expevt, uint32_t vaddr)
{
	bool miss = expevt == EXP_TLB_MISS_R || expevt == EXP_TLB_MISS_W;
	uint32_t spc = ctx->in_slot ? ctx->slot_pc - 2 : PC;

	IREG_PUT (4, CCN_TEA, vaddr);
	if (expevt != EXP_ADDR_ERR_R && expevt != EXP_ADDR_ERR_W)
		IREG_PUT (4, CCN_PTEH, (vaddr & 0xFFFFFC00) | (IREG_GET (4, CCN_PTEH) & 0xFF));
	ctx->in_slot = false;
	sh4_enter_exception (ctx, expevt, miss ? 0x400 : 0x100, spc);
	throw Sh4Fault ();
}

static const uint32_t tlb_page_masks[4] = { 0x3FF, 0xFFF, 0xFFFF, 0xFFFFF };

/* PTEL's layout, the UTLB data array's */
static void
tlb_set_data (Sh4::Tlb &e, uint32_t data)
{
	e.ppn = data & 0x1FFFFC00;
	e.v = (data >> 8) & 1;
	e.sz = ((data >> 6) & 2) | ((data >> 4) & 1);
	e.pr = (data >> 5) & 3;
	e.c = (data >> 3) & 1;
	e.d = (data >> 2) & 1;
	e.sh = (data >> 1) & 1;
	e.wt = data & 1;
	e.mask = tlb_page_masks[e.sz];
}

static uint32_t
tlb_data (const Sh4::Tlb &e)
{
	return e.ppn | (e.v << 8) | ((e.sz >> 1) << 7) | (e.pr << 5) |
	       ((e.sz & 1) << 4) | (e.c << 3) | (e.d << 2) | (e.sh << 1) | (uint32_t) e.wt;
}

static inline bool
tlb_match (const Sh4::Tlb &e, uint32_t vaddr, uint8_t asid, bool any_asid)
{
	return e.v && !((vaddr ^ e.vpn) & ~e.mask) && (e.sh || any_asid || e.asid == asid);
}

/* Whether PTEH's ASID can be ignored: MMUCR.SV in privileged mode */
static inline bool
tlb_any_asid (Sh4 *ctx)
{
	return (IREG_GET (4, CCN_MMUCR) & 0x100) && SR.bit.md;
}

static inline bool
tlb_translated (uint32_t vaddr)
{
	return vaddr < 0x80000000u || (vaddr >> 29) == 6;
}

/* The UTLB entry of vaddr, -1 for none; a lookup URC counts */
static int
utlb_find (Sh4 *ctx, uint32_t vaddr)
{
	uint8_t asid = IREG_GET (4, CCN_PTEH) & 0xFF;
	bool any = tlb_any_asid (ctx);
	unsigned urb = (IREG_GET (4, CCN_MMUCR) >> 18) & 63, i;

	ctx->mmu.urc = (ctx->mmu.urc + 1) & 63;
	if (urb && ctx->mmu.urc == urb)
		ctx->mmu.urc = 0;

	if (tlb_match (ctx->mmu.utlb[ctx->mmu.last_data], vaddr, asid, any))
		return ctx->mmu.last_data;
	for (i = 0; i < 64; i++)
		if (tlb_match (ctx->mmu.utlb[i], vaddr, asid, any)) {
			ctx->mmu.last_data = i;
			return i;
		}
	return -1;
}

/* A data access's physical address; P1, P2, P4 as they are */
static uint32_t
sh4_translate_data (Sh4 *ctx, uint32_t vaddr, bool write)
{
	int i;

	if (!tlb_translated (vaddr)) {
		bool sq = (vaddr >> 26) == (0xE0000000 >> 26);
		if (!SR.bit.md && !(sq && !(IREG_GET (4, CCN_MMUCR) & 0x200)))
			sh4_fault (ctx, write ? EXP_ADDR_ERR_W : EXP_ADDR_ERR_R, vaddr);
		return vaddr;
	}
	i = utlb_find (ctx, vaddr);
	if (i < 0)
		sh4_fault (ctx, write ? EXP_TLB_MISS_W : EXP_TLB_MISS_R, vaddr);

	const Sh4::Tlb &e = ctx->mmu.utlb[i];
	if ((!SR.bit.md && e.pr < 2) || (write && !(e.pr & 1)))
		sh4_fault (ctx, write ? EXP_TLB_PROT_W : EXP_TLB_PROT_R, vaddr);
	if (write && !e.d)
		sh4_fault (ctx, EXP_INITIAL_PAGE_W, vaddr);
	return (e.ppn & ~e.mask) | (vaddr & e.mask);
}

/* A fetch's physical address: the ITLB, filled from the UTLB on a miss */
static uint32_t
sh4_translate_fetch (Sh4 *ctx, uint32_t vaddr)
{
	uint8_t asid;
	bool any;
	unsigned k;

	if (!tlb_translated (vaddr)) {
		if (!SR.bit.md)
			sh4_fault (ctx, EXP_ADDR_ERR_R, vaddr);
		return vaddr;
	}
	asid = IREG_GET (4, CCN_PTEH) & 0xFF;
	any = tlb_any_asid (ctx);
	k = ctx->mmu.last_fetch;
	if (!tlb_match (ctx->mmu.itlb[k], vaddr, asid, any)) {
		for (k = 0; k < 4; k++)
			if (tlb_match (ctx->mmu.itlb[k], vaddr, asid, any))
				break;
		if (k == 4) {
			int i = utlb_find (ctx, vaddr);
			if (i < 0)
				sh4_fault (ctx, EXP_TLB_MISS_R, vaddr);
			k = ctx->mmu.itlb_next;
			ctx->mmu.itlb_next = (k + 1) & 3;
			ctx->mmu.itlb[k] = ctx->mmu.utlb[i];
			/* The ITLB keeps PR's user bit alone */
			ctx->mmu.itlb[k].pr &= 2;
		}
		ctx->mmu.last_fetch = k;
	}

	const Sh4::Tlb &e = ctx->mmu.itlb[k];
	if (!SR.bit.md && !(e.pr & 2))
		sh4_fault (ctx, EXP_TLB_PROT_R, vaddr);
	return (e.ppn & ~e.mask) | (vaddr & e.mask);
}

/* Whether the CPU can leave the MMU out: told it never faults (mmu_trusted),
 * and all its pages their own addresses, writable, but the store queues'
 * (whose addresses are looked up in any case). Smashing Drive's master: 200 MHz
 * of it in the interpreter is far from a frame's time */
static void
sh4_mmu_update (Sh4 *ctx)
{
	bool exact = ctx->mmu.on;
	unsigned i;

	if (exact && ctx->mmu_trusted) {
		exact = false;
		for (i = 0; i < 64 && !exact; i++) {
			const Sh4::Tlb &e = ctx->mmu.utlb[i];
			if (!e.v || (e.vpn >> 26) == (0xE0000000u >> 26))
				continue;
			exact = ((e.vpn ^ e.ppn) & ~e.mask & 0x1FFFFFFF) != 0 || e.pr != 3 || !e.d;
		}
	}
	if (exact != ctx->mmu_exact) {
		HL_CPU_LOG (ctx, "MMU %s", exact ? "exact" : "left out");
		ctx->mmu_exact = exact;
		ctx->mmu.flush_blocks = true;
	}
}

/* LDTLB: PTEH, PTEL and PTEA into the UTLB entry URC */
static void
sh4_ldtlb (Sh4 *ctx)
{
	Sh4::Tlb &e = ctx->mmu.utlb[ctx->mmu.urc];
	uint32_t pteh = (uint32_t) IREG_GET (4, CCN_PTEH);
	uint32_t ptea = (uint32_t) IREG_GET (4, CCN_PTEA);

	tlb_set_data (e, (uint32_t) IREG_GET (4, CCN_PTEL));
	e.vpn = pteh & 0xFFFFFC00;
	e.asid = pteh & 0xFF;
	e.sa = ptea & 7;
	e.tc = (ptea >> 3) & 1;
	HL_CPU_LOG (ctx, "LDTLB %u: %08X -> %08X (%08X)", ctx->mmu.urc,
	            e.vpn, e.ppn, tlb_data (e));
	sh4_mmu_update (ctx);
}

static void
sh4_mmucr_put (Sh4 *ctx, uint32_t val)
{
	bool on = val & 1;
	unsigned i;

	/* TI: all entries invalid; it reads 0 */
	if (val & 4) {
		for (i = 0; i < 64; i++)
			ctx->mmu.utlb[i].v = false;
		for (i = 0; i < 4; i++)
			ctx->mmu.itlb[i].v = false;
	}
	ctx->mmu.urc = (val >> 10) & 63;
	IREG_PUT (4, CCN_MMUCR, val & ~4);
	if (on != ctx->mmu.on) {
		HL_CPU_LOG (ctx, "MMU %s", on ? "on" : "off");
		ctx->mmu.on = on;
		ctx->mmu.flush_blocks = true;
	}
	sh4_mmu_update (ctx);
}

/* The TLBs' arrays (F2000000-F3FFFFFF, F6000000-F7FFFFFF); the caches'
 * (F0, F1, F4, F5) read 0 */
static uint32_t
sh4_tlb_array_get (Sh4 *ctx, uint32_t addr)
{
	bool array2 = (addr >> 23) & 1;

	switch (addr >> 24) {
	case 0xF2: {
		const Sh4::Tlb &e = ctx->mmu.itlb[(addr >> 8) & 3];
		return e.vpn | (e.v << 8) | e.asid;
	}
	case 0xF3: {
		const Sh4::Tlb &e = ctx->mmu.itlb[(addr >> 8) & 3];
		if (array2)
			return (e.tc << 3) | e.sa;
		return e.ppn | (e.v << 8) | ((e.sz >> 1) << 7) | ((e.pr >> 1) << 6) |
		       ((e.sz & 1) << 4) | (e.c << 3) | (e.sh << 1);
	}
	case 0xF6: {
		const Sh4::Tlb &e = ctx->mmu.utlb[(addr >> 8) & 63];
		return e.vpn | (e.d << 9) | (e.v << 8) | e.asid;
	}
	case 0xF7: {
		const Sh4::Tlb &e = ctx->mmu.utlb[(addr >> 8) & 63];
		return array2 ? (uint32_t) ((e.tc << 3) | e.sa) : tlb_data (e);
	}
	}
	return 0;
}

static void
sh4_tlb_array_put (Sh4 *ctx, uint32_t addr, uint32_t val)
{
	bool array2 = (addr >> 23) & 1;
	unsigned i;

	switch (addr >> 24) {
	case 0xF0:
	case 0xF1: {
		/* The instruction cache's address and data arrays. A written
		 * entry is left to be fetched again; associative (A, bit 3):
		 * the line of that tag, only */
		const unsigned line = Sh4ICache::index (addr);
		Sh4ICache &ic = ctx->blocks.icache;

		if ((addr >> 24) == 0xF1 || !(addr & 8) ||
		    ((ic.tags[line] << Sh4ICache::LINE_BITS) & 0x1FFFFC00) == (val & 0x1FFFFC00)) {
			ic.tags[line] = Sh4ICache::NONE;
			ic.held[line] = false;
			ctx->blocks.icache_changed ();
		}
		break;
	}
	case 0xF2: {
		Sh4::Tlb &e = ctx->mmu.itlb[(addr >> 8) & 3];
		e.vpn = val & 0xFFFFFC00;
		e.v = (val >> 8) & 1;
		e.asid = val & 0xFF;
		break;
	}
	case 0xF3: {
		Sh4::Tlb &e = ctx->mmu.itlb[(addr >> 8) & 3];
		if (array2) {
			e.tc = (val >> 3) & 1;
			e.sa = val & 7;
		} else {
			tlb_set_data (e, val & ~0x25);
			e.pr = ((val >> 6) & 1) << 1;
		}
		break;
	}
	case 0xF6:
		if (addr & 0x80) {
			/* Associative: D and V of the entries that match */
			uint32_t vpn = val & 0xFFFFFC00;
			uint8_t asid = val & 0xFF;
			bool any = tlb_any_asid (ctx);

			for (i = 0; i < 64; i++) {
				Sh4::Tlb &e = ctx->mmu.utlb[i];
				if (tlb_match (e, vpn, asid, any)) {
					e.d = (val >> 9) & 1;
					e.v = (val >> 8) & 1;
				}
			}
			for (i = 0; i < 4; i++) {
				Sh4::Tlb &e = ctx->mmu.itlb[i];
				if (tlb_match (e, vpn, asid, any))
					e.v = (val >> 8) & 1;
			}
		} else {
			Sh4::Tlb &e = ctx->mmu.utlb[(addr >> 8) & 63];
			e.vpn = val & 0xFFFFFC00;
			e.d = (val >> 9) & 1;
			e.v = (val >> 8) & 1;
			e.asid = val & 0xFF;
		}
		sh4_mmu_update (ctx);
		break;
	case 0xF7: {
		Sh4::Tlb &e = ctx->mmu.utlb[(addr >> 8) & 63];
		if (array2) {
			e.tc = (val >> 3) & 1;
			e.sa = val & 7;
		} else
			tlb_set_data (e, val);
		sh4_mmu_update (ctx);
		break;
	}
	default:
		/* The caches' arrays: not emulated */
		HL_CPU_LOG (ctx, "cache array W %08X = %08X", addr, val);
		break;
	}
}

/* Store Queues */

static uint32_t
get_sq_addr (Sh4 *ctx, uint32_t addr)
{
	uint32_t sq_num = (addr >> 5) & 1;
	uint32_t sq_base = (sq_num == 0) ?
	                   ctx->iregs->get (4, SHRINK (CCN_QACR0)) :
	                   ctx->iregs->get (4, SHRINK (CCN_QACR1));

	/* With the MMU, the UTLB's page (QACR's without an entry) */
	if (ctx->mmu.on) {
		int i = utlb_find (ctx, addr);
		if (i >= 0) {
			const Sh4::Tlb &e = ctx->mmu.utlb[i];
			return ((e.ppn & ~e.mask) | (addr & e.mask)) & ~0x1F;
		}
		HL_CPU_LOG (ctx, "store queue %08X: no UTLB entry", addr);
	}
	return ((sq_base & 0x1C) << 24) | (addr & 0x03FFFFE0);
}

static int
sh4_sq_get (Sh4 *ctx, unsigned size, uint32_t addr, void *val)
{
	uint32_t sq_addr = get_sq_addr (ctx, addr);
	return sh4_get (ctx, size, sq_addr | (addr & 0x1F), val);
}

static int
sh4_sq_put (Sh4 *ctx, unsigned size, uint32_t addr, uint64_t val)
{
	if (ctx->sq_buffered) {
		unsigned q = (addr >> 5) & 1, offs = addr & 0x1F;

		if (size == 8 && offs <= 24)
			memcpy ((uint8_t *) ctx->sq[q] + offs, &val, 8);
		else if (size <= 4 && offs + size <= 32)
			memcpy ((uint8_t *) ctx->sq[q] + offs, &val, size);
		return 0;
	}
	uint32_t sq_addr = get_sq_addr (ctx, addr) | (addr & 0x1F);
	return sh4_put (ctx, size, sq_addr | (addr & 0x1F), val);
}

void
Sh4::sq_flush (uint32_t addr)
{
	Sh4 *ctx = this;
	uint32_t target = get_sq_addr (ctx, addr);
	unsigned q = (addr >> 5) & 1, i;

	for (i = 0; i < 8; i++)
		((hl::Cpu *) ctx)->write (4, (target + i * 4) & 0x1FFFFFFF, ctx->sq[q][i]);
}

uint32_t
Sh4::ddt_source (unsigned ch)
{
	Sh4 *ctx = this;
	return IREG_GET (4, DMAC_SAR0 + ch * 0x10);
}

void
Sh4::ddt_done (unsigned ch, uint32_t source)
{
	Sh4 *ctx = this;
	uint32_t offs = ch * 0x10;

	IREG_PUT (4, DMAC_SAR0 + offs, source);
	IREG_PUT (4, DMAC_TCR0 + offs, 0);
	IREG_PUT (4, DMAC_CHCR0 + offs, IREG_GET (4, DMAC_CHCR0 + offs) | 2);
}

/* Bus Access */

#define IS_STORE_QUEUE(addr_) \
	(addr >= 0xE0000000 && addr <= 0xE3FFFFFF)

#define IS_ON_CHIP(addr_) \
	(((addr >> 24) == 0x1F) || \
	 ((addr >> 24) == 0xFF))

#define ADDR_MASK 0x1FFFFFFF

#define AREA(addr_) \
	(((addr_) >> 26) & 7)

/* The lines from pc to last come in. A copy held when the cache is on, the
 * area cached (not P2, P4) and the line in a RAM or ROM; the others noted as
 * fetched, so that their blocks do not come here again */
void
sh4_icache_fill (Sh4 *ctx, uint32_t pc, uint32_t last)
{
	Sh4ICache &ic = ctx->blocks.icache;
	hl::Mmap::Window *code = &ctx->code;
	const uint32_t area = pc >> 29;
	const bool cached = (IREG_GET (4, CCN_CCR) & 0x100) && area != 5 && area != 7;
	uint32_t a = pc & ADDR_MASK & ~((1u << Sh4ICache::LINE_BITS) - 1);

	last &= ADDR_MASK;
	for (; a <= last; a += 1u << Sh4ICache::LINE_BITS) {
		const unsigned i = Sh4ICache::index (a);

		if (ic.tags[i] == Sh4ICache::tag (a))
			continue;
		ic.tags[i] = Sh4ICache::tag (a);
		ic.held[i] = false;
		if (cached && ((a >= code->lo && a + 31 <= code->hi) || ctx->mmap->window (a, code)) &&
		    a + 31 <= code->hi) {
			memcpy (ic.data[i], code->base + (a & code->mask), sizeof (ic.data[i]));
			ic.held[i] = true;
		}
		ctx->blocks.line_replaced ();
	}
}

static int
sh4_fetch (Sh4 *ctx, uint32_t addr, uint16_t *inst)
{
	hl::Mmap::Window *code = &ctx->code;
	uint32_t a;
	int ret;

	if (ctx->mmu_exact)
		addr = sh4_translate_fetch (ctx, addr);
	a = addr & ADDR_MASK;

	if (!ctx->blocks.icache.present (a))
		sh4_icache_fill (ctx, addr, addr);
	{
		const uint16_t *line = ctx->blocks.icache.line (a);

		if (line) {
			*inst = line[(a >> 1) & (Sh4ICache::LINE_WORDS - 1)];
			return 0;
		}
	}

	/* From the RAM or ROM of the last fetch, without a lookup; another
	 * one becomes it */
	if ((a >= code->lo && a <= code->hi) || ctx->mmap->window (a, code)) {
		*inst = *(const uint16_t *) (code->base + (a & code->mask));
		return 0;
	}

	ret = ((hl::Cpu *) ctx)->read (2, a, (void *) inst);
	if (ret)
		HL_CPU_ABORT (ctx, "unhandled fetch @%08X", addr);
	return 0;
}

/* Logs an access nothing answered. A loop over unmapped memory would log
 * millions of lines, each flushed (SGNASCAR's loader copies 16 MB past its
 * ROM window, to fill the RAM up): the first accesses of each MB are logged,
 * then only the 1024th, 2048th, 4096th... All of them with -v. */
static void
log_unhandled (Sh4 *ctx, bool write, unsigned size, uint32_t addr, uint64_t val)
{
	uint64_t num = ++ctx->unhandled[addr >> 20];
	char count[96] = "";

	if (!hl_verbosity && num > 8 && (num < 1024 || (num & (num - 1))))
		return;
	if (num == 8)
		snprintf (count, sizeof (count), " (the next ones to %03Xxxxxx: the 1024th, 2048th...)",
		          addr >> 20);
	else if (num > 8)
		snprintf (count, sizeof (count), " (%llu to %03Xxxxxx so far)",
		          (unsigned long long) num, addr >> 20);
	if (write) {
		HL_CPU_ERROR (ctx, "unhandled W%d @%08X = %llX%s", 8*size, addr,
		              (unsigned long long) val, count);
	} else {
		HL_CPU_ERROR (ctx, "unhandled R%d @%08X%s", 8*size, addr, count);
	}
}

static int
sh4_get (Sh4 *ctx, unsigned size, uint32_t addr, void *val)
{
	int ret;

	if (IS_ON_CHIP (addr))
		ret = sh4_ireg_get (ctx, size, addr, val);
	else if (IS_STORE_QUEUE (addr))
		ret = sh4_sq_get (ctx, size, addr, val);
	else if (addr >= 0xF0000000 && addr < 0xF8000000) {
		/* The caches' and TLBs' arrays */
		set_ptr (val, size, sh4_tlb_array_get (ctx, addr));
		return 0;
	} else
		ret = ((hl::Cpu *) ctx)->read (size, addr & ADDR_MASK, val);
	/* TODO propagate to the caller to allow for memory exceptions */
	if (ret) {
		log_unhandled (ctx, false, size, addr, 0);
		/* Not what the caller's stack held: the same on each run */
		set_ptr (val, size, 0);
	}
	return ret;
}

static int
sh4_put (Sh4 *ctx, unsigned size, uint32_t addr, uint64_t val)
{
	int ret;

	ctx->stores++;
	if (addr - ctx->regs.r[15] + 0x200 >= 0x400)
		ctx->far_stores++;
	/* A store through the memory map, the on-chip DMA's, the store
	 * queues', the debugger's: on code compared in this epoch, a new one */
	ctx->blocks.written (addr, size);
	if (IS_ON_CHIP (addr))
		ret = sh4_ireg_put (ctx, size, addr, val);
	else if (addr >= 0xF0000000 && addr < 0xF8000000) {
		sh4_tlb_array_put (ctx, addr, (uint32_t) val);
		return 0;
	} else if (IS_STORE_QUEUE (addr))
		ret = sh4_sq_put (ctx, size, addr, val);
	else
		ret = ((hl::Cpu *) ctx)->write (size, addr & ADDR_MASK, val);
	/* TODO propagate to the caller to allow for memory exceptions */
	if (ret)
		log_unhandled (ctx, true, size, addr, val);
	if (!ctx->watchpoints.empty () && ctx->mach->hooks && ctx->watched (addr, size))
		ctx->mach->hooks->write (ctx, addr, size, val, PC);
	return ret;
}

/* The operand cache as RAM (CCR.ORA, with OCE): 8 KB in 7C000000-7FFFFFFF,
 * never translated; the half chosen by address bit 13 (bit 25 with OIX).
 * Smashing Drive's */
static inline uint8_t *
sh4_oc_ram (Sh4 *ctx, uint32_t addr, unsigned size)
{
	uint32_t ccr;

	if ((addr >> 26) != (0x7C000000 >> 26))
		return NULL;
	ccr = (uint32_t) IREG_GET (4, CCN_CCR);
	if ((ccr & 0x21) != 0x21)
		return NULL;
	return &ctx->oc_ram[((((ccr & 0x80) ? (addr >> 13) : (addr >> 1)) & 0x1000) |
	                     (addr & 0xFFF)) & ~(size - 1)];
}

/* The instructions' accesses: through the MMU when it is on */
#define DATA_ADDR(addr_, write_) \
	(ctx->mmu_exact ? sh4_translate_data (ctx, addr_, write_) : (addr_))

/* An access the memory map would take straight to a RAM or ROM, as the
 * translations' (sh4-x64.cpp): the MMU off, below P3, aligned, not the
 * operand cache's RAM; where it lies, NULL for the others */
static inline uint8_t *
sh4_direct (Sh4 *ctx, uint32_t addr, unsigned size, bool write)
{
	if (ctx->mmu_exact || addr >= 0xC0000000 || (addr & (size - 1)) ||
	    (addr >> 26) == (0x7C000000 >> 26))
		return NULL;
	return ((hl::Cpu *) ctx)->mmap->direct (addr & ADDR_MASK, write);
}

template <typename Word> static inline Word
sh4_read (Sh4 *ctx, uint32_t addr)
{
	Word tmp;
	uint8_t *p = sh4_direct (ctx, addr, sizeof (Word), false);

	if (!p)
		p = sh4_oc_ram (ctx, addr, sizeof (Word));
	if (p) {
		memcpy (&tmp, p, sizeof (Word));
		return tmp;
	}
	sh4_get (ctx, sizeof (Word), DATA_ADDR (addr, false), &tmp);
	return tmp;
}

/* A store straight to RAM is counted as sh4_put () counts it; none while a
 * write hook watches (sh4_put () calls it) */
template <typename Word> static inline void
sh4_write (Sh4 *ctx, uint32_t addr, Word val)
{
	uint8_t *p = ctx->watchpoints.empty () ? sh4_direct (ctx, addr, sizeof (Word), true) : NULL;

	if (p) {
		memcpy (p, &val, sizeof (Word));
		ctx->blocks.written (addr, sizeof (Word));
		ctx->stores++;
		if (addr - ctx->regs.r[15] + 0x200 >= 0x400)
			ctx->far_stores++;
		return;
	}
	p = sh4_oc_ram (ctx, addr, sizeof (Word));
	if (p) {
		memcpy (p, &val, sizeof (Word));
		ctx->stores++;
		if (addr - ctx->regs.r[15] + 0x200 >= 0x400)
			ctx->far_stores++;
		return;
	}
	sh4_put (ctx, sizeof (Word), DATA_ADDR (addr, true), val);
}

static inline uint8_t R8 (Sh4 *ctx, uint32_t addr) { return sh4_read<uint8_t> (ctx, addr); }
static inline uint16_t R16 (Sh4 *ctx, uint32_t addr) { return sh4_read<uint16_t> (ctx, addr); }
static inline uint32_t R32 (Sh4 *ctx, uint32_t addr) { return sh4_read<uint32_t> (ctx, addr); }
static inline uint64_t R64 (Sh4 *ctx, uint32_t addr) { return sh4_read<uint64_t> (ctx, addr); }

static inline void W8 (Sh4 *ctx, uint32_t addr, uint8_t val) { sh4_write<uint8_t> (ctx, addr, val); }
static inline void W16 (Sh4 *ctx, uint32_t addr, uint16_t val) { sh4_write<uint16_t> (ctx, addr, val); }
static inline void W32 (Sh4 *ctx, uint32_t addr, uint32_t val) { sh4_write<uint32_t> (ctx, addr, val); }
static inline void W64 (Sh4 *ctx, uint32_t addr, uint64_t val) { sh4_write<uint64_t> (ctx, addr, val); }

/* Interrupt Controller */

/**
 * Updates the intc.pending flag depending on whether an IRQ is pending or
 * not.
 */
static void
sh4_update_pending (Sh4 *ctx)
{
	unsigned priority, i;
	int maxi;
	uint16_t icr = IREG_GET (2, INTC_ICR);

	/* Default state: no IRQ pending */
	ctx->intc.pending = false;
	ctx->intc.index = -1;

	/* Handle NMI first. NMI is always accepted when the CPU is in SLEEP
	 * or STANDBY state, and when ICR.NMIB is set. It is blocked by BL
	 * only if ICR.NMIB is clear. */
	if (ctx->intc.irqs[SH4_IESOURCE_NMI].state == hl::IRQ_STATE_RAISED) {
		ctx->intc.pending = (SR.bit.bl == 0) || ((icr & 0x200) != 0);
		if (ctx->intc.pending) {
			ctx->intc.index = SH4_IESOURCE_NMI;
			return;
		}
	}

	/* TODO: ICR.MIE */

	/* All interrupts are blocked when SR.BL is set */
	if (SR.bit.bl)
		return;

	/* Find the highest priority raised IRQ; note that interrupt source
	 * numbers are sorted from highest to lowest priority. */
	priority = 0;
	for (i = 0; i < SH4_NUM_IESOURCES; i++) {
		/* TODO: handle ties like the hardware does */
		if (ctx->intc.irqs[i].state == hl::IRQ_STATE_RAISED &&
		    ctx->intc.irqs[i].priority > priority) {
			priority = ctx->intc.irqs[i].priority;
			maxi = i;
		}
	}
	/* Set the pending flag */
	if (priority > 0) {
		ctx->intc.pending = true;
		ctx->intc.index = maxi;
	}
}

/* intc.pending, and irq_ready: the one pending taken now (NMI's priority
 * above any SR.I) */
static void
sh4_update_irqs (Sh4 *ctx)
{
	sh4_update_pending (ctx);
	ctx->irq_ready = ctx->intc.pending &&
	                 ctx->intc.irqs[ctx->intc.index].priority > SR.bit.i;
}

/* sh4_iesource_t's */
static const char *const irq_names[] = {
	"NMI",
	"IRQ0", "IRQ1", "IRQ2", "IRQ3", "IRQ4", "IRQ5", "IRQ6", "IRQ7",
	"IRQ8", "IRQ9", "IRQ10", "IRQ11", "IRQ12", "IRQ13", "IRQ14",
	"IRL0", "IRL1", "IRL2", "IRL3",
	"UDI", "GPIOI",
	"DMTE0", "DMTE1", "DMTE2", "DMTE3", "DMAE",
	"TUNI0", "TUNI1", "TUNI2", "TICPI2",
	"ATI", "PRI", "CUI",
	"ERI", "RXI", "TXI", "TEI",
	"ERIF", "RXIF", "BRIF", "TXIF",
	"ITI",
	"RCMI", "ROVI",
};

static_assert (NUMELEM (irq_names) == SH4_NUM_IESOURCES, "an SH-4 interrupt source has no name");

const char *
Sh4::irq_name (unsigned num) const
{
	return (num < SH4_NUM_IESOURCES) ? irq_names[num] : "?";
}

int
Sh4::set_irq_state (unsigned num, hl::IrqState state, hl::Device *from)
{
	hl::Cpu *cpu = this;
	Sh4 *ctx = (Sh4 *) cpu;
	if (num >= SH4_NUM_IESOURCES)
		return -1;

	if (mach->hooks)
		mach->hooks->irq (from ? from : this, this, irq_names[num],
		                  state == hl::IRQ_STATE_RAISED);

	ctx->intc.irqs[num].state = state;

	/* Handle NMI */
	if (num == SH4_IESOURCE_NMI) {
		if (state == hl::IRQ_STATE_RAISED) {
			/* Set ICR.NMIL and DMAOR.NMIF */
			IREG_PUT (2, INTC_ICR, IREG_GET (2, INTC_ICR) | 0x8000);
			IREG_PUT (4, DMAC_DMAOR, IREG_GET (4, DMAC_DMAOR) | 2);
			/* Notify the DMAC that an NMI occurred */
			sh4_dmac_update_state (ctx, 0);
			sh4_dmac_run (ctx, 0x7FFFFFFF);
		} else {
			/* Clear ICR.NMIL; DMAOR.NMIF must be cleared
			 * manually by software. */
			IREG_PUT (2, INTC_ICR, IREG_GET (2, INTC_ICR) & 0x7FFF);
		}
	}

	/* Update the pending flag */
	sh4_update_irqs (ctx);
	return 0;
}

static void
sh4_process_irqs (hl::Cpu *cpu)
{
	Sh4 *ctx = (Sh4 *) cpu;
	int index;

	/* Check if there's something to do */
	if (!ctx->intc.pending)
		return;

	index = ctx->intc.index;
	if (ctx->intc.irqs[index].priority > SR.bit.i) {
		sh4_sr_t tmp;

		/* Standard interrupt context switch */
		SPC = PC;
		SSR = SR;
		SGR = R(15);

		PC = VBR + ctx->intc.irqs[index].offset;

		HL_CPU_LOG (ctx, "IRQ taken: SR.i=%X PRI=%X VBR=%08X offs=%X code=%X; jumping at %08X",
		            SR.bit.i, ctx->intc.irqs[index].priority,
		            VBR, ctx->intc.irqs[index].offset,
		            ctx->intc.irqs[index].code, PC);

		tmp.full = SR.full;
		tmp.bit.bl = 1;
		tmp.bit.md = 1;
		tmp.bit.rb = 1;
		set_sr (ctx, tmp.full);

		IREG_PUT (4, CCN_INTEVT, ctx->intc.irqs[index].code);

		/* Clear the interrupt source; TODO this is not correct, the
		 * source should be cleared externally! */
		ctx->intc.irqs[index].state = hl::IRQ_STATE_CLEAR;

		/* Update the pending flag */
		sh4_update_irqs (ctx);

		if (cpu->mach->hooks)
			cpu->mach->hooks->interrupt (cpu, irq_names[index], PC);
	}
}

/* Instructions */

static void delay_slot (Sh4 *ctx, uint32_t pc);

typedef void (* itype) (Sh4 *ctx, uint16_t inst);

#define I(name_) \
	static void sh4_interp_##name_ (Sh4 *ctx, uint16_t inst)

#define IDEF(mask_, match_, name_) \
	{ \
		mask_, \
		match_, \
		sh4_interp_##name_, \
	}

#define IS_SH4

#include "sh-insns-interp.h"
#include "sh-insns-desc.h"

#undef I
#undef IDEF
#undef IS_SH4

#define CHECK_COLLISION \
	do { \
		if (insns[inst] != sh4_interp_invalid) { \
			HL_LOG ("inst=%04X insns[inst]=%p", inst, insns[inst]); \
			HL_ABORT ("insns table collision"); \
		} \
	} while (0);

static void
setup_insns_handlers_from_table (const idesctype *desc,
                                 unsigned size)
{
	unsigned i, j;
	uint16_t inst;

	for (i = 0; i < size; i++) {
		switch (desc[i].mask) {
		case 0xF000:
			for (j = 0; j < 4096; j++) {
				inst = desc[i].match | j;
				CHECK_COLLISION;
				insns[inst] = desc[i].handler;
			}
			break;
		case 0xF00F:
			for (j = 0; j < 256; j++) {
				inst = desc[i].match | (j << 4);
				CHECK_COLLISION;
				insns[inst] = desc[i].handler;
			}
			break;
		case 0xFF00:
			for (j = 0; j < 256; j++) {
				inst = desc[i].match | j;
				CHECK_COLLISION;
				insns[inst] = desc[i].handler;
			}
			break;
		case 0xF08F:
			for (j = 0; j < 128; j++) {
				inst = desc[i].match |
				       ((j & 7) << 4) |
				       ((j >> 3) << 8);
				CHECK_COLLISION;
				insns[inst] = desc[i].handler;
			}
			break;
		case 0xF0FF:
			for (j = 0; j < 16; j++) {
				inst = desc[i].match | (j << 8);
				CHECK_COLLISION;
				insns[inst] = desc[i].handler;
			}
			break;
		case 0xF1FF:
			for (j = 0; j < 8; j++) {
				inst = desc[i].match | (j << 9);
				CHECK_COLLISION;
				insns[inst] = desc[i].handler;
			}
			break;
		case 0xF3FF:
			for (j = 0; j < 4; j++) {
				inst = desc[i].match | (j << 10);
				CHECK_COLLISION;
				insns[inst] = desc[i].handler;
			}
			break;
		case 0xFFFF:
			inst = desc[i].match;
			CHECK_COLLISION;
			insns[inst] = desc[i].handler;
			break;
		default:
			HL_ABORT ("unhandled mask %04X", desc[i].mask);
		}
	}
}

#undef CHECK_COLLISION

static void
setup_insns_handlers (void)
{
	unsigned i;

	for (i = 0; i < 65536; i++)
		insns[i] = sh4_interp_invalid;

	setup_insns_handlers_from_table (insns_desc_sh2, NUMELEM (insns_desc_sh2));
	setup_insns_handlers_from_table (insns_desc_sh4, NUMELEM (insns_desc_sh4));
}

Sh4Handler
sh4_handler (uint16_t inst)
{
	return insns[inst];
}

uint32_t
sh4_read32 (Sh4 *ctx, uint32_t addr)
{
	return R32 (ctx, addr);
}

void
sh4_write32 (Sh4 *ctx, uint32_t addr, uint32_t val)
{
	W32 (ctx, addr, val);
}

uint32_t
sh4_read8s (Sh4 *ctx, uint32_t addr)
{
	return (uint32_t) (int32_t) (int8_t) R8 (ctx, addr);
}

uint32_t
sh4_read16s (Sh4 *ctx, uint32_t addr)
{
	return (uint32_t) (int32_t) (int16_t) R16 (ctx, addr);
}

uint64_t
sh4_read64 (Sh4 *ctx, uint32_t addr)
{
	return R64 (ctx, addr);
}

void
sh4_write8 (Sh4 *ctx, uint32_t addr, uint32_t val)
{
	W8 (ctx, addr, (uint8_t) val);
}

void
sh4_write16 (Sh4 *ctx, uint32_t addr, uint32_t val)
{
	W16 (ctx, addr, (uint16_t) val);
}

void
sh4_write64 (Sh4 *ctx, uint32_t addr, uint64_t val)
{
	W64 (ctx, addr, val);
}

/* Execution */

static void
sh4_step (Sh4 *ctx, uint32_t pc)
{
	uint16_t inst;

	sh4_fetch (ctx, pc, &inst);

	inst = ((hl::Cpu *) ctx)->patch (pc & 0x1FFFFFFF, inst);

	insns[inst] (ctx, inst);

	ctx->remaining --;
}

void
Sh4::step_one ()
{
	sh4_step (this, regs.pc);
	regs.pc += 2;
}

static void
delay_slot (Sh4 *ctx, uint32_t pc)
{
	ctx->in_slot = true;
	ctx->slot_pc = pc;
	sh4_step (ctx, pc);
	ctx->in_slot = false;
}

/* Idle Loops
 *
 * The SH-4s spend most of their time waiting for a byte an interrupt sets:
 * SGNASCAR's master runs "mov.b @(0,gbr),r0; cmp/pl r0; bt" 89% of the
 * time, its slave a loop that also counts in r7 (and toggles a port bit
 * when r7 wraps) all the time. Within a CPU's slice nothing else runs: the
 * other CPU, the devices and the interrupts wait for its end, so a loop
 * whose iterations are alike stays so until then. Two alike in a row:
 *  - no store but below r15 (a function called: its return address, the
 *    registers it saves; the live stack above r15 the same after a turn as
 *    before), the same instructions taken (the path), the same registers
 *    but for some GPRs that each move by the same amount (counters);
 *  - a counter is only added to, never read otherwise on the path (no
 *    compare, no address): then where it stands changes nothing but itself;
 *    or it counts down alone, with dt (the delay loops "dt Rn; bf").
 * Such a loop is run forward: whole iterations are taken off the slice and
 * the counters moved as they would be, up to before an addc's carry (the
 * slave's port bit), before dt reaches 0. The timers count the whole slice
 * anyway (run ()). A loop's branch back may skip its body (MAX_SPAN: VON4's
 * waits on a ring buffer, 9 instructions over 58 bytes). A loop that calls
 * a function is looked at too, if its turn is short enough (MAX_PATH):
 * Smashing Drive's master waits for its frame calling one that polls the
 * board (0C05A88E), a fifth of the emulator's time. The loops that call
 * functions and read the timer are waited another way: see
 * sh4_tmu_polled (). The translations take three kinds
 * themselves, without this look: a delay loop "dt Rn; bf" alone in its
 * block (sh4-x64.cpp), a wait on memory "load; compare; branch"
 * (sh4_poll_skip ()), the turns of a loop let be (Sh4Block::quiet). Off
 * while profiling or with breakpoints; cpu:option ('idle', 0) turns it
 * off. */

void
Sh4::IdleLoop::start (Sh4 *ctx, uint32_t tail_, uint32_t head_)
{
	tail = tail_;
	head = head_;
	seen = 0;
	misses = 0;
	tracking = true;
	length = 0;
	last_length = 0;
	stores = ctx->stores;
	far_stores = ctx->far_stores;
	stack_held = false;
	regs = ctx->regs;
}

/* The live stack as a turn starts, for stack_kept (): held at the end of
 * the first turn that stored (a copy at each look's start took too long:
 * thousands a frame), the next turns compared with it */
void
Sh4::IdleLoop::hold_stack (Sh4 *ctx)
{
	uint32_t sp = ctx->regs.r[15];
	const uint8_t *at = (sp & 3) || sp >= 0xC0000000 ? NULL :
	                    ((hl::Cpu *) ctx)->mmap->direct (sp & ADDR_MASK, false);
	const uint8_t *end = at ? ((hl::Cpu *) ctx)->mmap->direct ((sp + STACK_KEPT - 4) & ADDR_MASK, false) : NULL;

	stack_held = at && end == at + STACK_KEPT - 4;
	stack_sp = sp;
	if (stack_held)
		memcpy (stack, at, STACK_KEPT);
}

/* The turn's stores, all near the stack, were below r15 where it started
 * (dead once it is over): the live stack as it was, r15 back where it was */
bool
Sh4::IdleLoop::stack_kept (Sh4 *ctx) const
{
	const uint8_t *at;

	if (!stack_held || ctx->regs.r[15] != stack_sp)
		return false;
	at = ((hl::Cpu *) ctx)->mmap->direct (stack_sp & ADDR_MASK, false);
	return at && !memcmp (stack, at, STACK_KEPT);
}

void
Sh4::IdleLoop::back_edge (Sh4 *ctx, uint32_t tail_, uint32_t head_)
{
	unsigned i;
	bool same_regs, alike, known_turn, stored, stack_new;
	Known &known_one = known_of (tail_, head_);

	/* A loop found busy is let be, looked at again every so often (the
	 * slave's writes once, its port bit, in millions of iterations, and
	 * stays); the one noted, around it, given up */
	if (known_one.tail == tail_ && known_one.head == head_ && known_one.dead) {
		tracking = false;
		if (--known_one.dead == 0)
			start (ctx, tail_, head_);
		return;
	}

	if (tail_ != tail || head_ != head) {
		/* Started again and again, never a turn closed (another loop's
		 * way back each time between: two loops one within the other,
		 * which started each other again, never let be; SWRACER's
		 * master noted 3600 blocks a frame so): let be as busy */
		if (known_one.tail != tail_ || known_one.head != head_) {
			known_one = Known ();
			known_one.tail = tail_;
			known_one.head = head_;
		}
		if (++known_one.opened >= 8) {
			known_one.opened = 0;
			let_be (tail_, head_);
			return;
		}
		start (ctx, tail_, head_);
		return;
	}
	known_one.opened = 0;
	stored = ctx->stores != stores;
	if (!tracking || ctx->far_stores != far_stores ||
	    (stored && stack_held && ctx->regs.r[15] == stack_sp && !stack_kept (ctx))) {
		/* It writes (but to the dead stack), or is too long: not an
		 * idle loop */
		let_be (tail_, head_);
		return;
	}
	/* The first turn that stored: the live stack held from here, the
	 * turn not alike (the next one compared) */
	stack_new = stored && !stack_kept (ctx);

	/* The same path, the same registers but the GPRs, which move as in
	 * the iteration before (the registers compared once for both looks:
	 * the comparisons took a tenth of the idle slaves' time) */
	same_regs = !memcmp (&ctx->regs.pc, &regs.pc, sizeof (regs) - offsetof (decltype (regs), pc));
	alike = same_regs && !stack_new && length == last_length &&
	        !memcmp (path, last_path, length * sizeof (path[0]));
	for (i = 0; alike && i < 16; i++)
		alike = (int32_t) (ctx->regs.r[i] - regs.r[i]) == delta[i];
	for (i = 0; i < 16; i++)
		delta[i] = (int32_t) (ctx->regs.r[i] - regs.r[i]);
	seen = alike ? seen + 1 : 0;
	misses = alike ? 0 : misses + 1;
	/* The turn of a loop run forward before: at once; looked at when it
	 * serves (not alike, or for the plan of one run forward) */
	known_turn = same_regs && !stack_new && (!alike || seen >= 2) &&
	             known_one.tail == tail_ && known_one.head == head_ &&
	             known_one.length && known_one.length == length &&
	             !memcmp (delta, known_one.delta, sizeof (delta)) &&
	             !memcmp (path, known_one.path, length * sizeof (path[0]));
	if (!alike && known_turn)
		seen = 2;

	/* Busy, though it stores nothing: never alike (a search, a sum), or
	 * alike but never run forward (a counter read): looked at again
	 * later, as one that writes; the look at each turn took a tenth of
	 * the SH-4s' time in the games' boots */
	if (seen >= 2) {
		Plan plan;
		int ran = run_forward (ctx, known_turn && known_one.plan_epoch == ctx->blocks.epoch ?
		                            &known_one : NULL, &plan);


		/* Run forward: its turn kept */
		if (ran > 0) {
			if (known_one.tail != tail_ || known_one.head != head_) {
				known_one.tail = tail_;
				known_one.head = head_;
				known_one.dead = 0;
			}
			known_one.busy = 0;
			known_one.length = length;
			memcpy (known_one.path, path, length * sizeof (path[0]));
			memcpy (known_one.delta, delta, sizeof (delta));
			known_one.plan = plan;
			known_one.plan_epoch = ctx->blocks.epoch;
		}
		if (ran < 0) {
			let_be (tail_, head_);
			return;
		}
	}
	if (misses >= 8) {
		let_be (tail_, head_);
		return;
	}

	regs = ctx->regs;
	memcpy (last_path, path, length * sizeof (path[0]));
	last_length = length;
	length = 0;
	/* The next turn's stores counted from here */
	stores = ctx->stores;
	if (stack_new)
		hold_stack (ctx);
}

void
Sh4::IdleLoop::restart (Sh4 *ctx, uint32_t tail_, uint32_t head_)
{
	Known &known_one = known_of (tail_, head_);

	/* One let be counts its turn down all the same */
	if (known_one.tail == tail_ && known_one.head == head_ && known_one.dead)
		back_edge (ctx, tail_, head_);
	else
		start (ctx, tail_, head_);
}

void
Sh4::IdleLoop::let_be (uint32_t t, uint32_t h)
{
	Known &k = known_of (t, h);

	if (k.tail != t || k.head != h) {
		k.tail = t;
		k.head = h;
		k.busy = 0;
		k.length = 0;
	}
	k.dead = 64u << k.busy;
	if (k.busy < 4)
		k.busy++;
	tracking = false;
}

/* The instruction reads or writes r0 without naming it: the indexed and
 * displacement moves, those of GBR, MOVA, MOVCA.L, the logic with an
 * immediate, CMP/EQ #imm and the FPU's indexed moves */
static bool
uses_r0 (uint16_t op)
{
	switch (op & 0xF00F) {
	case 0x0004: case 0x0005: case 0x0006:	/* mov.x Rm,@(R0,Rn) */
	case 0x000C: case 0x000D: case 0x000E:	/* mov.x @(R0,Rm),Rn */
	case 0xF006: case 0xF007:		/* fmov @(R0,Rm), @(R0,Rn) */
		return true;
	}
	if ((op & 0xF0FF) == 0x00C3)		/* movca.l R0,@Rn */
		return true;
	switch (op & 0xFF00) {
	case 0x8000: case 0x8100:		/* mov.x R0,@(disp,Rn) */
	case 0x8400: case 0x8500:		/* mov.x @(disp,Rm),R0 */
	case 0x8800:				/* cmp/eq #imm,R0 */
		return true;
	}
	/* C0-C2, C4-CF: GBR's moves, MOVA, the logic with #imm (C3: trapa) */
	return (op & 0xF000) == 0xC000 && (op & 0x0F00) != 0x0300;
}

/* 1 when run forward; 0 not now (near the slice's end, a countdown's);
 * -1 never (a counter read otherwise). known_turn: the same turn's, its
 * plan found before; plan: this one's */
int
Sh4::IdleLoop::run_forward (Sh4 *ctx, const Known *known_turn, Plan *plan)
{
	uint32_t counters = 0, carried = 0, added = 0, counted = 0, compared = 0;
	int64_t k;
	unsigned i, c;

	for (i = 0; i < 16; i++)
		if (delta[i])
			counters |= 1u << i;
	/* Each instruction of the path: a counter added to, or not there; r0
	 * not used without being named either (SWRACER's delay loop "dt r0;
	 * bf" ran, each turn through sh4_chain ()) */
	for (i = 0; i < length && !known_turn; i++) {
		const uint8_t *at = ((hl::Cpu *) ctx)->mmap->direct (path[i] & ADDR_MASK, false);
		uint16_t op;
		unsigned n, m;

		if (at)
			memcpy (&op, at, sizeof (op));
		else if (ctx->read (2, path[i] & ADDR_MASK, &op))
			return -1;
		if ((counters & 1) && uses_r0 (op))
			return -1;
		n = (op >> 8) & 15;
		m = (op >> 4) & 15;
		if ((op & 0xF000) == 0x7000) {
			/* add #imm,Rn */
			added |= 1u << n;
			continue;
		} else if ((op & 0xF00F) == 0x300C || (op & 0xF00F) == 0x300E) {
			/* add Rm,Rn, addc Rm,Rn */
			if (counters & (1u << m))
				return -1;
			if ((op & 0xF00F) == 0x300E)
				carried |= counters & (1u << n);
			added |= 1u << n;
			continue;
		} else if ((op & 0xF0FF) == 0x4010) {
			/* dt Rn: a countdown, T when it reaches 0 */
			counted |= counters & (1u << n);
			continue;
		} else if ((op & 0xF0FF) == 0x4015 || (op & 0xF0FF) == 0x4011 ||
		           ((op & 0xF00F) == 0x2008 && n == m)) {
			/* cmp/pl Rn, cmp/pz Rn, tst Rn,Rn: a counter compared
			 * with zero, a wait's time out (SWRACER's on its sound
			 * board's mailbox, 2700 turns a frame) */
			compared |= counters & (1u << n);
			continue;
		}
		if ((counters & (1u << n)) || (counters & (1u << m)))
			return -1;
	}

	/* A countdown counts down alone */
	if (counted & added)
		return -1;
	if (known_turn) {
		carried = known_turn->plan.carried;
		counted = known_turn->plan.counted;
		compared = known_turn->plan.compared;
	}
	plan->carried = carried;
	plan->counted = counted;
	plan->compared = compared;

	/* Whole iterations, the one under way left to run */
	k = ctx->remaining / (int) length - 1;
	for (c = 0; c < 16 && k > 0; c++) {
		if (carried & (1u << c)) {
			/* An addc's carry comes when the counter wraps: stop
			 * short */
			if (delta[c] <= 0)
				return -1;
			k = MIN2 (k, (int64_t) ((0xFFFFFFFFu - ctx->regs.r[c]) / (uint32_t) delta[c]) - 2);
		}
		if (counted & (1u << c)) {
			/* dt sets T at 0: stop short of it (the delay loops
			 * "dt Rn; bf", SGNASCAR's 8192 a frame); one that
			 * did set it is not alike */
			if (delta[c] != -1)
				return -1;
			if (ctx->regs.r[c] < 3)
				return 0;
			k = MIN2 (k, (int64_t) ctx->regs.r[c] - 2);
		}
		if (compared & (1u << c)) {
			/* The comparison the same to the end: one counted
			 * down stops short of zero */
			if (delta[c] >= 0)
				return -1;
			if ((int32_t) ctx->regs.r[c] <= 0)
				return 0;
			k = MIN2 (k, (int64_t) ((int32_t) ctx->regs.r[c] / -delta[c]) - 2);
		}
	}
	if (k <= 0)
		return 0;

	for (c = 0; c < 16; c++)
		ctx->regs.r[c] += (uint32_t) (k * delta[c]);
	ctx->remaining -= (int) (k * length);
	ctx->cycles_skipped += (uint64_t) (k * length);
	return 1;
}

/* After an instruction at pc: the idle loops' look at it */
static inline void
sh4_watch_idle (Sh4 *ctx, uint32_t pc, int before)
{
	ctx->idle.step (pc, before - ctx->remaining);
	/* A short way back */
	if (PC <= pc && pc - PC < Sh4::IdleLoop::MAX_SPAN) {
		if (ctx->remaining > 16)
			ctx->idle.back_edge (ctx, pc, PC);
		else
			ctx->idle.restart (ctx, pc, PC);
	}
}

/* Chaining
 *
 * A translation's end goes on to the next block's translation itself,
 * without the run loop, when the loop would only call it: no interrupt to
 * take, the CPU running, the MMU off, the next block decoded and its code
 * still in memory, translated, the slice with room for it. Some 40 000
 * blocks a frame of each SH-4, of 8 instructions or so: the loop's lookups
 * between them took as long as the blocks. The idle loops keep their look,
 * taken here as the run loop takes it (idle_look ()): the turn noted, a
 * short way back that stored nothing (a loop that may only wait) looked at
 * but for one found busy, while it is let be. The translations' probe
 * leaves the blocks to this call while a turn is noted (it took 8000 trips
 * a frame back to the run loop in SWRACER). HR_SH4_CHAIN=0 turns it off. */

/* After a translation of n instructions from pc0 (the cycles left before
 * it, before): the idle loops' look at it, the run loop's or sh4_chain ()'s,
 * the same */
static inline void
idle_look (Sh4 *ctx, uint32_t pc0, int n, int before, bool chained)
{
	/* The last one took a delay slot's cycle too (its handler's) */
	int slot = before - ctx->remaining - n;
	uint32_t last = pc0 + 2 * (uint32_t) (n - 1);
	bool back = PC <= last && last - PC < Sh4::IdleLoop::MAX_SPAN;


	/* A wait on memory whose turns were taken off (sh4_poll_skip ()), a
	 * delay loop's taken by its translation at once (sh4-x64.cpp): no turn
	 * of its own to look at, the one noted given up */
	if (ctx->chain.polled == pc0) {
		ctx->chain.polled = ~0u;
		ctx->idle.drop ();
		return;
	}
	if (slot > 1 && ctx->idle.noting ()) {
		const Sh4Block *block = ctx->blocks.peek (pc0);

		if (block && block->delay_loop) {
			ctx->idle.drop ();
			return;
		}
	}
	if (ctx->idle.noting ()) {
		for (int i = 0; i < n; i++)
			ctx->idle.step (pc0 + 2 * (uint32_t) i, i == n - 1 ? 1 + slot : 1);
	} else if (chained && !(back && ctx->stores == ctx->chain.stores))
		/* Gone along without a look: a loop that stores */
		return;
	else if (chained && !ctx->idle.watches (last, PC)) {
		/* One let be: its translation's way back goes on without
		 * this call for its other turns (a busy loop that stores
		 * nothing came here at each turn: 3000 times a frame in
		 * SWRACER) */
		Sh4Block *block = ctx->blocks.peek (pc0);

		if (block)
			block->quiet = ctx->idle.hand_over (last, PC);
		return;
	}
	if (back && ctx->remaining > 16)
		ctx->idle.back_edge (ctx, last, PC);
	else if (back)
		ctx->idle.restart (ctx, last, PC);
}

/* A wait on memory, "load; compare; bt or bf" back to the block's start
 * (Sh4Block::poll_loop: SWRACER's master waits on a byte so at each slice,
 * "mov.b @(0,gbr),r0; cmp/pl r0; bt"), at its translation's way back: the
 * turn that went back, the same as all the others to the slice's end (it
 * stores nothing, nothing else runs, its load reads a RAM or ROM in place,
 * as the translations' fast paths do: no device). They are taken off at
 * once, but the last cycles (one at least): what the idle loops did after
 * two turns noted and compared, their look at each slice. Not while the
 * idle loops are off */
int
sh4_poll_skip (Sh4 *ctx, uint32_t inst)
{
	const unsigned m = (inst >> 4) & 15, disp = inst & 15;
	static const unsigned sizes[4] = { 1, 2, 4, 4 };
	uint32_t addr, size;
	int k;

	if (!ctx->chain.idle)
		return 0;
	switch (inst & 0xF000) {
	case 0xC000:				/* mov.x @(disp,GBR),R0 */
		size = 1u << ((inst >> 8) & 3);
		addr = ctx->regs.gbr + (inst & 0xFF) * size;
		break;
	case 0x8000:				/* mov.x @(disp,Rm),R0 */
		size = 1u << ((inst >> 8) & 1);
		addr = ctx->regs.r[m] + disp * size;
		break;
	case 0x5000:				/* mov.l @(disp,Rm),Rn */
		size = 4;
		addr = ctx->regs.r[m] + disp * 4;
		break;
	default:				/* mov.x @Rm,Rn */
		size = sizes[inst & 3];
		addr = ctx->regs.r[m];
		break;
	}
	if (addr >= 0xC0000000 || (addr & ADDR_MASK) >= 0x1C000000 || (addr & (size - 1)) ||
	    !((hl::Cpu *) ctx)->mmap->direct (addr & ADDR_MASK, false))
		return 0;
	/* The cycles left once the turn that ran is counted (its 3) */
	k = (ctx->remaining - 3 - 1) / 3;
	if (k <= 0)
		return 0;
	ctx->remaining -= 3 * k;
	ctx->cycles_skipped += (uint64_t) (3 * k);
	ctx->chain.polled = ctx->regs.pc;
	return 1;
}

uintptr_t
sh4_chain (Sh4 *ctx, uint32_t pc0, int n)
{
	hl::Cpu *cpu = ctx;
	Sh4Block *block;
	void *body;

	ctx->chain.pc0 = pc0;
	ctx->chain.n = n;
	if (!ctx->chain.enabled || cpu->state != hl::CPU_STATE_RUN || ctx->mmu_exact ||
	    ctx->irq_ready)
		return 0;
	if (ctx->chain.idle) {
		idle_look (ctx, pc0, n, ctx->chain.before, true);
		/* Told: the run loop looks no more */
		ctx->chain.n = 0;
	}
	block = ctx->blocks.peek (PC);
	if (!block || cpu->remaining <= (int) block->ops.size ())
		return 0;
	body = ctx->jit->body (block);
	if (!body)
		return 0;
	ctx->jit->note (PC, block, ctx->blocks.epoch);
	ctx->chain.stores = ctx->stores;
	ctx->chain.before = cpu->remaining;
	ctx->chain.links++;
	ctx->chain.gone++;
	return (uintptr_t) body;
}

/* run ()'s loop over decoded blocks (sh4-blocks.h): the same steps, an
 * instruction at a time, in the same order (an interrupt taken before any
 * of them, the slice's cycles counted down, the patches), the fetches and
 * the decoding left out */
static void
sh4_run_blocks (Sh4 *ctx, bool skip_idle)
{
	hl::Cpu *cpu = ctx;
	bool irqs_done = false;

	ctx->chain.idle = skip_idle;

	/* The backend for the memory map's way (sh4-x64.h): a new one when it
	 * changed, at a frame's start */
	if (ctx->use_jit && (!ctx->jit || !ctx->jit->fits (cpu->mmap))) {
		ctx->jit.reset (Sh4X64::create (cpu->mmap));
		if (!ctx->jit)
			ctx->use_jit = false;
	}

	/* The MMU on: the interpreter alone (see "MMU") */
	while (cpu->remaining > 0 && cpu->state == hl::CPU_STATE_RUN && !ctx->mmu_exact) {
		Sh4Block *block;
		size_t i;

		if (!irqs_done)
			sh4_process_irqs (cpu);
		irqs_done = false;

		block = ctx->blocks.get (ctx, PC);

		/* Its x86-64 translation, when the slice has room for all of it
		 * (and a delay slot): the same steps, the idle loops told of
		 * them afterwards (of the last block, when it went on to others:
		 * none noted then) */
		if (block && ctx->use_jit && cpu->remaining > (int) block->ops.size ()) {
			Sh4Native native;

			native = ctx->jit ? ctx->jit->get (ctx, block) : NULL;
			if (native) {
				int before = cpu->remaining;
				int n;

				ctx->jit->note (PC, block, ctx->blocks.epoch);
				ctx->chain.stores = ctx->stores;
				ctx->chain.before = before;
				ctx->chain.links = 0;
				n = native (ctx, PC);
				/* The last block, unless sh4_chain () looked at
				 * it (n 0 then) */
				if (skip_idle && n != 0)
					idle_look (ctx, ctx->chain.pc0, n, ctx->chain.before, false);
				continue;
			}
		}

		if (!block) {
			/* Code outside a RAM or ROM: fetched */
			uint32_t pc = PC;
			int before = cpu->remaining;

			sh4_step (ctx, pc);
			PC += 2;
			if (skip_idle)
				sh4_watch_idle (ctx, pc, before);
			continue;
		}

		sh4_icache_fill (ctx, PC, PC + 2 * block->num_words - 1);
		for (i = 0; ; ) {
			const Sh4Op &op = block->ops[i];
			uint32_t pc = PC;
			int before = cpu->remaining;

			if (op.patched) {
				uint16_t inst = (uint16_t) cpu->patch (pc & ADDR_MASK, op.inst);
				(inst == op.inst ? op.handler : insns[inst]) (ctx, inst);
			} else
				op.handler (ctx, op.inst);
			cpu->remaining--;
			PC += 2;
			if (skip_idle)
				sh4_watch_idle (ctx, pc, before);

			/* Left on a branch, at the block's end, at the slice's,
			 * when the MMU comes on */
			if (PC != pc + 2 || ++i == block->ops.size () ||
			    cpu->remaining <= 0 || cpu->state != hl::CPU_STATE_RUN ||
			    ctx->mmu_exact)
				break;
			/* An interrupt taken: its handler's first instruction
			 * next, from its own block */
			sh4_process_irqs (cpu);
			if (PC != pc + 2) {
				irqs_done = true;
				break;
			}
		}
	}
}

int
Sh4::run (int cycles)
{
	hl::Cpu *cpu = this;
	Sh4 *ctx = (Sh4 *) cpu;
	/* The translations' stores call no write hook */
	bool debugging = cpu->profile || !cpu->breakpoints.empty () ||
	                 !cpu->watchpoints.empty ();
	bool skip_idle = idle.enabled && !debugging;

	/* Stopped at a breakpoint: nothing runs, the timers neither */
	if (cpu->break_hit)
		return 0;

	cpu->remaining = cycles;
	/* The MMU switched: the blocks decoded before, dropped */
	if (mmu.flush_blocks) {
		blocks.clear ();
		mmu.flush_blocks = false;
	}
	if (slice_hook && skip_idle && !mmu_exact && !irq_ready && cpu->state == hl::CPU_STATE_RUN)
		slice_hook (ctx);
	/* The debugger's breakpoints and profile look at each fetch */
	if (use_blocks && !debugging && !mmu_exact)
		sh4_run_blocks (ctx, skip_idle);
	while (cpu->remaining > 0 && cpu->state == hl::CPU_STATE_RUN) {
		uint32_t pc;
		int before;

		sh4_process_irqs (cpu);
		if (!cpu->breakpoints.empty () && cpu->at_breakpoint (PC))
			return 0;
		if (cpu->profile)
			(*cpu->profile)[PC & ADDR_MASK]++;
		pc = PC;
		before = cpu->remaining;
		if (mmu_exact) {
			/* An exception unwinds the instruction: its handler
			 * next (see "MMU") */
			try {
				sh4_step (ctx, pc);
				PC += 2;
			} catch (const Sh4Fault &) {
				cpu->remaining--;
			}
		} else {
			sh4_step (ctx, pc);
			PC += 2;
		}
		if (skip_idle)
			sh4_watch_idle (ctx, pc, before);
	}
	cycles_run += (uint64_t) (cycles - cpu->remaining);
	/* XXX BSC, SCI */
	sh4_tmu_run (ctx, cycles);
	//sh4_dmac_run (ctx, cycles);
	return -cpu->remaining;
}

void
Sh4::set_state (hl::CpuState state)
{
	hl::Cpu *cpu = this;
	/* TODO: standby, deep sleep, etc. */
	cpu->state = state;
}

/* See Table 19.5, "Interrupt Exception Handling Sources and
 * Priority Orders".
 *
 * Note that the indices of this array are ordered from highest priority
 * to lowest. */

static const sh4_irq_state_t default_irq_state[SH4_NUM_IESOURCES] = {
	{ hl::IRQ_STATE_CLEAR, 16, 0x600, 0x1C0 },
	/* IRQs: the IRL pins' levels, encoded (ICR.IRLM clear): IRQn is the
	 * pins at n, level 15 - n, as Gaelco's boards raise them */
	{ hl::IRQ_STATE_CLEAR, 15, 0x600, 0x200 },
	{ hl::IRQ_STATE_CLEAR, 14, 0x600, 0x220 },
	{ hl::IRQ_STATE_CLEAR, 13, 0x600, 0x240 },
	{ hl::IRQ_STATE_CLEAR, 12, 0x600, 0x260 },
	{ hl::IRQ_STATE_CLEAR, 11, 0x600, 0x280 },
	{ hl::IRQ_STATE_CLEAR, 10, 0x600, 0x2A0 },
	{ hl::IRQ_STATE_CLEAR,  9, 0x600, 0x2C0 },
	{ hl::IRQ_STATE_CLEAR,  8, 0x600, 0x2E0 },
	{ hl::IRQ_STATE_CLEAR,  7, 0x600, 0x300 },
	{ hl::IRQ_STATE_CLEAR,  6, 0x600, 0x320 },
	{ hl::IRQ_STATE_CLEAR,  5, 0x600, 0x340 },
	{ hl::IRQ_STATE_CLEAR,  4, 0x600, 0x360 },
	{ hl::IRQ_STATE_CLEAR,  3, 0x600, 0x380 },
	{ hl::IRQ_STATE_CLEAR,  2, 0x600, 0x3A0 },
	{ hl::IRQ_STATE_CLEAR,  1, 0x600, 0x3C0 },
	/* IRLs */
	{ hl::IRQ_STATE_CLEAR, 13, 0x600, 0x240 },
	{ hl::IRQ_STATE_CLEAR, 10, 0x600, 0x2A0 },
	{ hl::IRQ_STATE_CLEAR,  7, 0x600, 0x300 },
	{ hl::IRQ_STATE_CLEAR,  4, 0x600, 0x360 },
	/* UDI */
	{ hl::IRQ_STATE_CLEAR, -1, 0x600, 0x600 },
	/* GPIO */
	{ hl::IRQ_STATE_CLEAR, -1, 0x600, 0x620 },
	/* DMAC */
	{ hl::IRQ_STATE_CLEAR, -1, 0x600, 0x640 },
	{ hl::IRQ_STATE_CLEAR, -1, 0x600, 0x660 },
	{ hl::IRQ_STATE_CLEAR, -1, 0x600, 0x680 },
	{ hl::IRQ_STATE_CLEAR, -1, 0x600, 0x6A0 },
	{ hl::IRQ_STATE_CLEAR, -1, 0x600, 0x6C0 },
	/* TMU */
	{ hl::IRQ_STATE_CLEAR, -1, 0x600, 0x400 },
	{ hl::IRQ_STATE_CLEAR, -1, 0x600, 0x420 },
	{ hl::IRQ_STATE_CLEAR, -1, 0x600, 0x440 },
	{ hl::IRQ_STATE_CLEAR, -1, 0x600, 0x460 },
	/* RTC */
	{ hl::IRQ_STATE_CLEAR, -1, 0x600, 0x480 },
	{ hl::IRQ_STATE_CLEAR, -1, 0x600, 0x4A0 },
	{ hl::IRQ_STATE_CLEAR, -1, 0x600, 0x4C0 },
	/* SCI1 */
	{ hl::IRQ_STATE_CLEAR, -1, 0x600, 0x4E0 },
	{ hl::IRQ_STATE_CLEAR, -1, 0x600, 0x500 },
	{ hl::IRQ_STATE_CLEAR, -1, 0x600, 0x520 },
	{ hl::IRQ_STATE_CLEAR, -1, 0x600, 0x540 },
	/* SCIF */
	{ hl::IRQ_STATE_CLEAR, -1, 0x600, 0x700 },
	{ hl::IRQ_STATE_CLEAR, -1, 0x600, 0x720 },
	{ hl::IRQ_STATE_CLEAR, -1, 0x600, 0x740 },
	{ hl::IRQ_STATE_CLEAR, -1, 0x600, 0x760 },
	/* WDT */
	{ hl::IRQ_STATE_CLEAR, -1, 0x600, 0x560 },
	/* REF */
	{ hl::IRQ_STATE_CLEAR, -1, 0x600, 0x580 },
	{ hl::IRQ_STATE_CLEAR, -1, 0x600, 0x5A0 },
};

void
Sh4::reset (hl::ResetType type)
{
	hl::Device *dev = this;
	hl::Cpu *cpu = (hl::Cpu *) dev;
	Sh4 *ctx = (Sh4 *) cpu;
	uint32_t tmp;

	cpu->state = (ctx->config.master) ?
	              hl::CPU_STATE_RUN :
	              hl::CPU_STATE_STOP;

	ctx->in_slot = false;
	ctx->idle.reset ();
	ctx->blocks.clear ();

	memset ((void *) &ctx->regs, 0, sizeof (ctx->regs));
	/* MMUCR clear: the MMU off, the TLBs' entries undefined (invalid) */
	memset ((void *) &ctx->mmu, 0, sizeof (ctx->mmu));
	ctx->mmu_exact = false;

	PC = 0xA0000000;

	SR.full = 0;
	SR.bit.i = 0xF;
	SR.bit.bl = 1;
	SR.bit.rb = 1;
	SR.bit.md = 1;

	FPSCR.full = 0;
	FPSCR.bit.rm = 1;
	FPSCR.bit.dn = 1;

	/* See Table A.1, "Address List" */
	ctx->iregs->clear ();

	tmp = (ctx->config.master ? 0x40000000 : 0) |
	      (ctx->config.little_endian) ? 0x80000000 : 0;

	IREG_PUT (4, CCN_EXPEVT, (type == hl::RESET_HARD) ? 0 : 0x20);
	IREG_PUT (4, BSC_BCR1, tmp);
	IREG_PUT (2, BSC_BCR2, 0x3FFC);
	IREG_PUT (4, BSC_WCR1, 0x77777777);
	IREG_PUT (4, BSC_WCR2, 0xFFFEEFFF);
	IREG_PUT (4, BSC_WCR3, 0x07777777);
	IREG_PUT (4, TMU_TCOR0, 0xFFFFFFFF);
	IREG_PUT (4, TMU_TCNT0, 0xFFFFFFFF);
	IREG_PUT (4, TMU_TCOR1, 0xFFFFFFFF);
	IREG_PUT (4, TMU_TCNT1, 0xFFFFFFFF);
	IREG_PUT (4, TMU_TCOR2, 0xFFFFFFFF);
	IREG_PUT (4, TMU_TCNT2, 0xFFFFFFFF);
	IREG_PUT (1, SCI_SCBRR1, 0xFF);
	IREG_PUT (1, SCI_SCTDR1, 0xFF);
	IREG_PUT (1, SCI_SCSSR1, 0x84);
	IREG_PUT (1, SCIF_SCBRR2, 0xFF);
	IREG_PUT (2, SCIF_SCFSR2, 0x0060);
	IREG_PUT (2, UDI_SDIR, 0xFFFF);

	ctx->intc.pending = false;
	ctx->intc.index = -1;
	ctx->irq_ready = false;
	HL_ASSERT (sizeof (ctx->intc.irqs) == sizeof (default_irq_state));
	memcpy (ctx->intc.irqs, default_irq_state, sizeof (default_irq_state));

	memset ((void *) &ctx->dmac, 0, sizeof (ctx->dmac));

	memset ((void *) &ctx->tmu, 0, sizeof (ctx->tmu));
	ctx->tmu.counter[0] = 0xFFFFFFFF;
	ctx->tmu.counter[1] = 0xFFFFFFFF;
	ctx->tmu.counter[2] = 0xFFFFFFFF;
}

#define SAVE(thing_) \
		ret |= state->put ((void *) &(thing_), sizeof (thing_))

#define LOAD(thing_) \
		ret |= state->get ((void *) &(thing_), sizeof (thing_))

int
Sh4::load_state (hl::State *state)
{
	hl::Device *dev = this;
	Sh4 *ctx = (Sh4 *) dev;
	int ret = 0;

	/* Running, or asleep */
	LOAD (ctx->state);
	LOAD (ctx->in_slot);
	LOAD (ctx->regs);
	ctx->idle.reset ();
	LOAD (ctx->intc);
	ctx->irq_ready = ctx->intc.pending &&
	                 ctx->intc.irqs[ctx->intc.index].priority > SR.bit.i;
	LOAD (ctx->dmac);
	LOAD (ctx->tmu);
	LOAD (ctx->config);
	LOAD (ctx->mmu);
	LOAD (ctx->slot_pc);
	LOAD (ctx->oc_ram);
	if (ctx->scif_pipe) {
		LOAD (ctx->scif_rx);
		LOAD (ctx->scif_rx_count);
		LOAD (ctx->scif_rx_credit);
		LOAD (ctx->scif_status);
		LOAD (ctx->scif_status_read);
	}
	sh4_mmu_update (ctx);
	ctx->mmu.flush_blocks = true;

	return ret;
}

int
Sh4::save_state (hl::State *state)
{
	hl::Device *dev = this;
	Sh4 *ctx = (Sh4 *) dev;
	int ret = 0;

	SAVE (ctx->state);
	SAVE (ctx->in_slot);
	SAVE (ctx->regs);
	SAVE (ctx->intc);
	SAVE (ctx->dmac);
	SAVE (ctx->tmu);
	SAVE (ctx->config);
	SAVE (ctx->mmu);
	SAVE (ctx->slot_pc);
	SAVE (ctx->oc_ram);
	if (ctx->scif_pipe) {
		SAVE (ctx->scif_rx);
		SAVE (ctx->scif_rx_count);
		SAVE (ctx->scif_rx_credit);
		SAVE (ctx->scif_status);
		SAVE (ctx->scif_status_read);
	}

	return ret;
}

#undef SAVE
#undef LOAD

bool
Sh4::set_option (const char *name, int value)
{
	if (!strcmp (name, "idle")) {
		idle.enabled = value != 0;
		idle.reset ();
		return true;
	}
	if (!strcmp (name, "blocks")) {
		use_blocks = value != 0;
		blocks.clear ();
		return true;
	}
	if (!strcmp (name, "jit")) {
		use_jit = value != 0;
		return true;
	}
	if (!strcmp (name, "pages")) {
		mmap->set_page_table (value != 0);
		return true;
	}
	if (!strcmp (name, "fastmem"))
		return mmap->set_fastmem (value != 0);
	if (!strcmp (name, "dynarec")) {
		if (use_blocks != (value != 0))
			blocks.clear ();
		use_blocks = use_jit = value != 0;
		return true;
	}
	return false;
}

Sh4::~Sh4 ()
{
	/* HR_PROFILE: what the idle loops took off */
	if (hl::Profile::enabled && cycles_run)
		HL_PRINT ("sh4 %c: %llu cycles, %llu run, %llu skipped by the idle loops (%.1f%%)",
		          config.master ? 'M' : 'S', (unsigned long long) cycles_run,
		          (unsigned long long) (cycles_run - cycles_skipped),
		          (unsigned long long) cycles_skipped, 100.0 * (double) cycles_skipped / (double) cycles_run);
	if (hl::Profile::enabled && jit)
		HL_PRINT ("sh4 %c: %u blocks decoded, %u translations in %.0f ms (the arena cleared %u times), %u code epochs",
		          config.master ? 'M' : 'S', blocks.decoded (), jit->translated (),
		          1000.0 * jit->translating_time (), jit->clears (), blocks.epoch);
	if (hl::Profile::enabled && chain.ends)
		HL_PRINT ("sh4 %c: %llu translated blocks run, %llu of them gone on to the next (%.1f%%)",
		          config.master ? 'M' : 'S', (unsigned long long) chain.ends,
		          (unsigned long long) chain.gone, 100.0 * (double) chain.gone / (double) chain.ends);
}

const char *
Sh4::debug_string ()
{
	hl::Cpu *cpu = this;
	Sh4 *ctx = (Sh4 *) cpu;
	static char str[256];
	sprintf (str, "%c @%08X @%08X %08X",
	         ctx->config.master ? 'M' : 'S', PC, PR, SR.full);
	return str;
}

/* Debugger */

static const char *const register_names[] = {
	"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
	"r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
	"pc", "pr", "sr", "gbr", "vbr", "dbr", "mach", "macl",
	"spc", "ssr", "sgr", "fpul", "fpscr",
	"fr0", "fr1", "fr2", "fr3", "fr4", "fr5", "fr6", "fr7",
	"fr8", "fr9", "fr10", "fr11", "fr12", "fr13", "fr14", "fr15",
	"xf0", "xf1", "xf2", "xf3", "xf4", "xf5", "xf6", "xf7",
	"xf8", "xf9", "xf10", "xf11", "xf12", "xf13", "xf14", "xf15",
	"rbank0", "rbank1", "rbank2", "rbank3", "rbank4", "rbank5", "rbank6", "rbank7",
};

enum {
	REG_PC = 16, REG_PR, REG_SR, REG_GBR, REG_VBR, REG_DBR, REG_MACH, REG_MACL,
	REG_SPC, REG_SSR, REG_SGR, REG_FPUL, REG_FPSCR,
	REG_FR0, REG_XF0 = REG_FR0 + 16, REG_RBANK0 = REG_XF0 + 16
};

/* Where a register is kept; SR and FPSCR are written through set_sr () and
 * set_fpscr (), which switch the banks */
static uint32_t *
register_ptr (Sh4 *ctx, unsigned index)
{
	if (index < 16)
		return &R(index);
	if (index >= REG_RBANK0)
		return &RBANK(index - REG_RBANK0);
	if (index >= REG_XF0)
		return &XF(index - REG_XF0).u;
	if (index >= REG_FR0)
		return &FR(index - REG_FR0).u;
	switch (index) {
	case REG_PC:	return &PC;
	case REG_PR:	return &PR;
	case REG_SR:	return &SR.full;
	case REG_GBR:	return &GBR;
	case REG_VBR:	return &VBR;
	case REG_DBR:	return &DBR;
	case REG_MACH:	return &MACH;
	case REG_MACL:	return &MACL;
	case REG_SPC:	return &SPC;
	case REG_SSR:	return &SSR.full;
	case REG_SGR:	return &SGR;
	case REG_FPUL:	return &FPUL.u;
	default:	return &FPSCR.full;
	}
}

unsigned
Sh4::num_registers () const
{
	return NUMELEM (register_names);
}

const char *
Sh4::register_name (unsigned index) const
{
	return (index < NUMELEM (register_names)) ? register_names[index] : NULL;
}

uint64_t
Sh4::get_register (unsigned index)
{
	Sh4 *ctx = this;
	if (index >= NUMELEM (register_names))
		return 0;
	return *register_ptr (ctx, index);
}

void
Sh4::set_register (unsigned index, uint64_t val)
{
	Sh4 *ctx = this;
	if (index >= NUMELEM (register_names))
		return;
	if (index == REG_SR)
		set_sr (ctx, (uint32_t) val);
	else if (index == REG_FPSCR)
		set_fpscr (ctx, (uint32_t) val);
	else
		*register_ptr (ctx, index) = (uint32_t) val;
}

int
Sh4::debug_read (unsigned size, uint32_t addr, void *val)
{
	return sh4_get (this, size, addr, val);
}

int
Sh4::debug_write (unsigned size, uint32_t addr, uint64_t val)
{
	return sh4_put (this, size, addr, val);
}

void
sh4_set_porta_handlers (hl::Cpu *cpu,
                        int (* get)(Sh4 *ctx, uint16_t *val),
                        int (* put)(Sh4 *ctx, uint16_t val))
{
	Sh4 *ctx = (Sh4 *) cpu;

	ctx->porta.get = get;
	ctx->porta.put = put;
}

Sh4::Sh4 (hl::Machine *mach, hl::Mmap *mmap, bool master, bool le) :
	hl::Cpu (mach, mmap)
{
	config.master = master;
	config.little_endian = le;
	/* P0 to P3 are views of the same physical memory */
	addr_mask = ADDR_MASK;

	iregs = hl::Buffer::create (0x10000, 0, hl::Buffer::LE32);
	HL_ASSERT (iregs);
	mach->register_buffer (iregs);

	chain.enabled = hl_util_get_bool_option ("HR_SH4_CHAIN", true);
	blocks.checking = hl_util_get_bool_option ("HR_SH4_EPOCH_CHECK", false);
	setup_insns_handlers ();
}
