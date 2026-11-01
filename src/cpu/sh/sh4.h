/* 
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/cpu/sh/sh4.h, Copyright (C) 2011, 2012, Stefano Teso.
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

#ifndef __SH4_H__
#define __SH4_H__

#include <string>
#include <unordered_map>

#include "hl/core.h"
#include "hl/buffer.h"
#include "hl/cpu.h"
#include "cpu/sh/sh4-blocks.h"

class Sh4X64;

typedef union {
	struct {
		uint32_t t  : 1;
		uint32_t s  : 1;
		uint32_t    : 2;
		uint32_t i  : 4;
		uint32_t q  : 1;
		uint32_t m  : 1;
		uint32_t    : 5;
		uint32_t fd : 1;	/* FPU disable */
		uint32_t    : 12;
		uint32_t bl : 1;	/* Exception/Interrupt block */
		uint32_t rb : 1;	/* Register bank */
		uint32_t md : 1;	/* Processor mode */
		uint32_t    : 1;
	} bit;
	uint32_t full;
} sh4_sr_t;

typedef union {
	struct {
		uint32_t rm		: 2; /** Rounding mode */
		uint32_t flag		: 5; /** Exception flag */
		uint32_t enable		: 5; /** Exception enable */
		uint32_t cause		: 6; /** Exception cause */
		uint32_t dn		: 1; /** Denormalization mode */
		uint32_t pr		: 1; /** Precision mode */
		uint32_t sz		: 1; /** Transfer size */
		uint32_t fr		: 1; /** Floating-point register bank */
	} bit;
	uint32_t full;
} sh4_fpscr_t;

typedef enum {
	SH4_IESOURCE_NMI,
	/* IRQs */
	SH4_IESOURCE_IRQ0,
	SH4_IESOURCE_IRQ1,
	SH4_IESOURCE_IRQ2,
	SH4_IESOURCE_IRQ3,
	SH4_IESOURCE_IRQ4,
	SH4_IESOURCE_IRQ5,
	SH4_IESOURCE_IRQ6,
	SH4_IESOURCE_IRQ7,
	SH4_IESOURCE_IRQ8,
	SH4_IESOURCE_IRQ9,
	SH4_IESOURCE_IRQ10,
	SH4_IESOURCE_IRQ11,
	SH4_IESOURCE_IRQ12,
	SH4_IESOURCE_IRQ13,
	SH4_IESOURCE_IRQ14,
	/* IRLs */
	SH4_IESOURCE_IRL0,
	SH4_IESOURCE_IRL1,
	SH4_IESOURCE_IRL2,
	SH4_IESOURCE_IRL3,
	/* UDI */
	SH4_IESOURCE_UDI,
	/* GPIO */
	SH4_IESOURCE_GPIOI,
	/* DMAC */
	SH4_IESOURCE_DMTE0,
	SH4_IESOURCE_DMTE1,
	SH4_IESOURCE_DMTE2,
	SH4_IESOURCE_DMTE3,
	SH4_IESOURCE_DMAE,
	/* TMU */
	SH4_IESOURCE_TUNI0,
	SH4_IESOURCE_TUNI1,
	SH4_IESOURCE_TUNI2,
	SH4_IESOURCE_TICPI2,
	/* RTC */
	SH4_IESOURCE_ATI,
	SH4_IESOURCE_PRI,
	SH4_IESOURCE_CUI,
	/* SCI1 */
	SH4_IESOURCE_ERI,
	SH4_IESOURCE_RXI,
	SH4_IESOURCE_TXI,
	SH4_IESOURCE_TEI,
	/* SCIF */
	SH4_IESOURCE_ERIF,
	SH4_IESOURCE_RXIF,
	SH4_IESOURCE_BRIF,
	SH4_IESOURCE_TXIF,
	/* WDT */
	SH4_IESOURCE_ITI,
	/* REF */
	SH4_IESOURCE_RCMI,
	SH4_IESOURCE_ROVI,

	SH4_NUM_IESOURCES,
} sh4_iesource_t;

typedef struct {
	hl::IrqState state;
	unsigned priority;
	uint32_t offset;
	uint32_t code;
} sh4_irq_state_t;

/* The Hitachi SH-4, interpreted; see sh4.cpp */
class Sh4 : public hl::Cpu {
public:
	Sh4 (hl::Machine *mach, hl::Mmap *mmap, bool master, bool le);

	int set_irq_state (unsigned num, hl::IrqState state, hl::Device *from = NULL) override;
	const char *irq_name (unsigned num) const override;
	int run (int cycles) override;
	void set_state (hl::CpuState state) override;
	void reset (hl::ResetType type) override;
	int load_state (hl::State *state) override;
	int save_state (hl::State *state) override;
	const char *debug_string () override;
	/* "idle", 0 or 1: the idle loops run forward (sh4.cpp); "blocks", 0
	 * or 1: the code runs from decoded blocks (sh4-blocks.h); "jit", 0 or
	 * 1: the blocks translated to x86-64 (sh4-x64.h); "dynarec", 0 or 1:
	 * both, or the interpreter alone (the settings' choice); "pages", 0
	 * or 1: its memory map's page table (Mmap); "fastmem", 0 or 1: its
	 * RAM and ROM shown in the host's address space (Mmap) */
	bool set_option (const char *name, int value) override;

	unsigned num_registers () const override;
	const char *register_name (unsigned index) const override;
	uint64_t get_register (unsigned index) override;
	void set_register (unsigned index, uint64_t val) override;
	/* P4 included: the on-chip registers and the store queues */
	int debug_read (unsigned size, uint32_t addr, void *val) override;
	int debug_write (unsigned size, uint32_t addr, uint64_t val) override;

	/* State */
	bool		in_slot;

	/* Registers */
	struct {
		uint32_t	r[16];
		uint32_t	pc;
		sh4_sr_t	sr;
		uint32_t	pr;
		uint32_t	gbr;
		uint32_t	vbr;
		uint32_t	dbr;
		pair32u_t	mac;
		uint32_t	spc;
		sh4_sr_t	ssr;
		uint32_t	sgr;
		uint32_t	rbank[8];
		union {
			alias32uf_t f[16];
			alias64uf_t d[8];
		} f, x;
		alias32uf_t	fpul;
		sh4_fpscr_t	fpscr;
	} regs;

	/* On-Chip Modules */
	hl::Buffer	*iregs;

	struct {
		/* True if any interrupt is pending */
		bool pending;
		/* Index of the highest priority raised interrupt */
		int index;
		/* Overall interrupt state object */
		sh4_irq_state_t irqs[SH4_NUM_IESOURCES];
	} intc;

	struct {
		bool	is_running[4];
	} dmac;

	struct {
		bool	is_running[3];
		uint32_t counter[3];
		/* A tick's fraction not counted yet, in CPU cycles times
		 * 8 x the tick's rate */
		uint64_t cycles[3];
	} tmu;

	struct {
		int	(* get)(Sh4 *ctx, uint16_t *val);
		int	(* put)(Sh4 *ctx, uint16_t val);
	} porta;

	/* The MMU (sh4.cpp, "MMU"): its TLBs, used while MMUCR.AT is set */
	struct Tlb {
		uint32_t vpn;	/* the page's virtual address */
		uint32_t ppn;	/* its physical address, 29 bits */
		uint32_t mask;	/* its offset's bits: 3FF, FFF, FFFF or FFFFF */
		uint8_t	asid;
		uint8_t	sz;	/* 0 1 KB, 1 4 KB, 2 64 KB, 3 1 MB */
		uint8_t	pr;	/* 0 privileged read, 1 privileged read and
				 * write, 2 read, 3 read and write; the
				 * ITLB's: 0 privileged, 2 all */
		uint8_t	sa, tc;	/* PCMCIA's */
		bool	v, d, c, sh, wt;
	};
	struct {
		bool	on;		/* MMUCR.AT */
		Tlb	utlb[64];
		Tlb	itlb[4];
		unsigned urc;		/* MMUCR.URC, the UTLB entry LDTLB fills */
		unsigned itlb_next;	/* the ITLB entry replaced next */
		/* The entries last hit, looked at first */
		unsigned last_data, last_fetch;
		/* The decoded blocks to drop before running them again */
		bool	flush_blocks;
	} mmu;
	/* The delay slot run (in_slot): its address, for the exceptions */
	uint32_t	slot_pc;
	/* What the SCIF sent since the last line, for the log */
	std::string	scif_line;
	/* The operand cache's half used as RAM (CCR.ORA, sh4_oc_ram ()) */
	uint8_t		oc_ram[8192];

	/* Configuration */
	struct {
		bool	master;
		bool	little_endian;
	} config;

	/* The accesses nothing answered, per MB (address >> 20), for the log */
	std::unordered_map<uint32_t, uint64_t> unhandled;

	/* The CPU's stores, counted: an idle loop has none; those away from
	 * the stack (beyond 512 bytes of r15), which a loop polling a timer
	 * through its calls has none of */
	uint64_t	stores = 0;
	uint64_t	far_stores = 0;
	/* The clock its cycles run at, in Hz (the machine's), which the timers
	 * count against: 200 MHz on the board */
	uint32_t	cpu_hz = 200000000;
	/* The last TCNT read: where from (PC, PR, r15), the far stores then */
	struct {
		uint32_t pc, pr, sp;
		uint64_t far_stores;
	} poll = {};

	/* The RAM or ROM the code was last fetched from (sh4_fetch) */
	hl::Mmap::Window code = { 1, 0, 0, NULL };

	/* The code decoded in blocks, run by run () while use_blocks; their
	 * x86-64 translations while use_jit too */
	Sh4BlockCache	blocks;
	bool		use_blocks = true;
	bool		use_jit = true;
	std::unique_ptr<Sh4X64> jit;

	~Sh4 ();

	/* A loop that waits for the rest of the machine (a flag an interrupt
	 * sets): its iterations alike, it is run forward to the end of the
	 * slice, nothing else running meanwhile. See sh4.cpp */
	class IdleLoop {
	public:
		static const unsigned MAX_PATH = 24;
		/* How far back a loop's branch may go, in bytes: its path may
		 * skip a body (VON4's 0C711ABE, 58 bytes for 9 instructions) */
		static const unsigned MAX_SPAN = 256;

		bool		 enabled = true;

		void		 reset () { tail = ~0u; }
		/* An iteration's path being noted: step () does nothing
		 * otherwise */
		bool		 noting () const { return tracking; }
		/* Each instruction run, with the instructions it took (2 with
		 * a delay slot) */
		void		 step (uint32_t pc, int taken)
		{
			if (tracking && length + taken <= MAX_PATH) {
				path[length++] = pc;
				if (taken == 2)
					path[length++] = pc + 2;
			} else if (tracking)
				tracking = false;
		}
		/* A branch from tail back to head */
		void		 back_edge (Sh4 *ctx, uint32_t tail, uint32_t head);

	private:
		uint32_t	 head = 0, tail = ~0u;
		bool		 tracking = false;
		unsigned	 dead = 0;	/* iterations before a new look */
		unsigned	 seen = 0;
		uint64_t	 stores = 0;
		/* The instructions of the last iteration, and the one before */
		unsigned	 length = 0, last_length = 0;
		uint32_t	 path[MAX_PATH], last_path[MAX_PATH];
		/* The registers at the head, and how the GPRs moved */
		decltype (Sh4::regs) regs;
		int32_t		 delta[16];

		void		 start (Sh4 *ctx, uint32_t tail, uint32_t head);
		void		 run_forward (Sh4 *ctx);
	} idle;
};

void		 sh4_set_porta_handlers (hl::Cpu *cpu,
		                         int (* get)(Sh4 *ctx, uint16_t *val),
		                         int (* put)(Sh4 *ctx, uint16_t val));

#endif /* __SH4_H__ */
