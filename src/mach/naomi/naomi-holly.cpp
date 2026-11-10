/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from MAME's src/mame/sega/dc.cpp, maple-dc.cpp, naomig1.cpp and
 * src/devices/video/powervr2.cpp (license BSD-3-Clause, copyright-holders
 * Samuele Zannoli, R. Belmont, ElSemi, Olivier Galibert).
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
#include <cstring>

#include "hl/core.h"
#include "hl/machine.h"

#include "cpu/sh/sh4.h"
#include "mach/naomi/naomi.h"

namespace naomi {

/*
 * The Holly
 * =========
 *
 * The chip beside the SH-4: its system bus (005F6800), the Maple bus (005F6C00),
 * the G1 bus and the cartridge (005F7000, 005F7400), the G2 bus and the AICA
 * (005F7800), the PowerVR 2 (005F7C00, 005F8000), its Tile Accelerator's FIFO
 * and the paths to the VRAM (area 4).
 *
 * Interrupts: three banks of causes (normal, external, error), each with a mask
 * for the SH-4's levels 6, 4 and 2, its IRL pins at 9, B and D.
 *
 * The Tile Accelerator
 * ====================
 *
 * The games send it lists of parameters, 32 or 64 bytes each, whose first word
 * tells what it is:
 *
 *  31-29 0 end of list, 1 a tile clip, 2 another list's start, 4 a polygon's or
 *	  a modifier volume's header, 5 a sprite's, 7 a vertex
 *  28	  a strip ends here
 *  26-24 at a list's first header, the list: 0 opaque, 1 opaque modifiers, 2
 *	  translucent, 3 translucent modifiers, 4 punch-through
 *  23	  the next three fields are valid: 17-16 the tile clip's mode
 *  7-0	  the polygon: 7 shadowed, 6 two volumes, 5-4 the colours (0 packed, 1
 *	  floats, 2 intensity with this header's colour, 3 intensity with the
 *	  last one), 3 textured, 2 offset colour, 1 Gouraud, 0 16-bit UVs
 *
 * On the chip, it writes object lists and vertices to the VRAM, per tile, and
 * the core reads them back. Here, its triangles are collected at once: a list
 * is drawn when STARTRENDER names it, by the address it was made at.
 */

enum {
	SB_C2DSTAT	= 0x00 / 4,
	SB_C2DLEN	= 0x04 / 4,
	SB_C2DST	= 0x08 / 4,
	SB_SDST		= 0x20 / 4,
	SB_LMMODE0	= 0x84 / 4,
	SB_LMMODE1	= 0x88 / 4,
	SB_FFST		= 0x8C / 4,
	SB_SFRES	= 0x90 / 4,
	SB_SBREV	= 0x9C / 4,
	SB_ISTNRM	= 0x100 / 4,
	SB_ISTEXT	= 0x104 / 4,
	SB_ISTERR	= 0x108 / 4,
	SB_IML2NRM	= 0x110 / 4,
	SB_IML2EXT	= 0x114 / 4,
	SB_IML2ERR	= 0x118 / 4,
	SB_IML4NRM	= 0x120 / 4,
	SB_IML4EXT	= 0x124 / 4,
	SB_IML4ERR	= 0x128 / 4,
	SB_IML6NRM	= 0x130 / 4,
	SB_IML6EXT	= 0x134 / 4,
	SB_IML6ERR	= 0x138 / 4,
};

enum {
	IST_EOR_VIDEO	= 1 << 0,
	IST_EOR_ISP	= 1 << 1,
	IST_EOR_TSP	= 1 << 2,
	IST_VBL_IN	= 1 << 3,
	IST_VBL_OUT	= 1 << 4,
	IST_HBL_IN	= 1 << 5,
	IST_EOXFER_YUV	= 1 << 6,
	IST_EOXFER_OP	= 1 << 7,
	IST_EOXFER_OPMV	= 1 << 8,
	IST_EOXFER_TR	= 1 << 9,
	IST_EOXFER_TRMV	= 1 << 10,
	IST_DMA_PVR	= 1 << 11,
	IST_DMA_MAPLE	= 1 << 12,
	IST_DMA_GDROM	= 1 << 14,
	IST_DMA_AICA	= 1 << 15,
	IST_DMA_CH2	= 1 << 19,
	IST_EOXFER_PT	= 1 << 21,
	IST_EXT		= 1 << 30,
	IST_ERROR	= 1u << 31,

	IST_BOTH_CLX	= IST_EOR_VIDEO | IST_EOR_ISP | IST_EOR_TSP | IST_EOXFER_OP | IST_EOXFER_OPMV |
			  IST_EOXFER_TR | IST_EOXFER_TRMV | IST_EOXFER_PT,
};

/* The PowerVR's registers */
enum {
	PVR_ID		= 0x000 / 4,
	PVR_REVISION	= 0x004 / 4,
	PVR_SOFTRESET	= 0x008 / 4,
	PVR_STARTRENDER	= 0x014 / 4,
	PVR_PARAM_BASE	= 0x020 / 4,
	PVR_REGION_BASE	= 0x02C / 4,
	PVR_FB_W_CTRL	= 0x048 / 4,
	PVR_FB_W_STRIDE	= 0x04C / 4,
	PVR_FB_W_SOF1	= 0x060 / 4,
	PVR_FB_X_CLIP	= 0x068 / 4,
	PVR_FB_Y_CLIP	= 0x06C / 4,
	PVR_SHAD_SCALE	= 0x074 / 4,
	PVR_CULL_VAL	= 0x078 / 4,
	PVR_PARAM_CFG	= 0x07C / 4,
	PVR_BACKGND_D	= 0x088 / 4,
	PVR_BACKGND_T	= 0x08C / 4,
	PVR_FEED_CFG	= 0x098 / 4,
	PVR_FOG_COL_RAM	= 0x0B0 / 4,
	PVR_FOG_COL_VERT = 0x0B4 / 4,
	PVR_FOG_DENSITY	= 0x0B8 / 4,
	PVR_CLAMP_MAX	= 0x0BC / 4,
	PVR_CLAMP_MIN	= 0x0C0 / 4,
	PVR_HBLANK_INT	= 0x0C8 / 4,
	PVR_VBLANK_INT	= 0x0CC / 4,
	PVR_SPG_LOAD	= 0x0D8 / 4,
	PVR_TEXT_CONTROL = 0x0E4 / 4,
	PVR_SCALER_CTL	= 0x0F4 / 4,
	PVR_PAL_CTRL	= 0x108 / 4,
	PVR_SPG_STATUS	= 0x10C / 4,
	PVR_PT_ALPHA	= 0x11C / 4,
	PVR_TA_OL_BASE	= 0x124 / 4,
	PVR_TA_ISP_BASE	= 0x128 / 4,
	PVR_TA_NEXT_OPB	= 0x134 / 4,
	PVR_TA_ITP	= 0x138 / 4,
	PVR_LIST_INIT	= 0x144 / 4,
	PVR_YUV_BASE	= 0x148 / 4,
	PVR_YUV_CTRL	= 0x14C / 4,
	PVR_YUV_CNT	= 0x150 / 4,
	PVR_LIST_CONT	= 0x160 / 4,
	PVR_OPB_INIT	= 0x164 / 4,
	PVR_FOG_TABLE	= 0x200 / 4,
	PVR_PALETTE	= 0x1000 / 4,
};

enum {
	MP_IDLE, MP_SEND, MP_WAIT_REPLY, MP_WAIT_NOP, MP_TIMEOUT, MP_GOT_REPLY, MP_DONE
};

static const uint32_t VRAM_MASK = 0x00FFFFFF;

/* The Maple's timeout, a line short of 5 ms */
static const int MAPLE_TIMEOUT_LINES = 150;
static const int MAPLE_DONE_LINES = 6;

/* The lines from STARTRENDER to its end, for the games' frame loops */
static const unsigned RENDER_LINES = 12;

static inline float
to_float (uint32_t val)
{
	float f;
	memcpy (&f, &val, 4);
	return f;
}

static inline uint32_t
to_byte (float f)
{
	return f <= 0.0f ? 0 : f >= 1.0f ? 255 : (uint32_t) (f * 255.0f + 0.5f);
}

static inline uint32_t
argb_float (float a, float r, float g, float b)
{
	return (to_byte (a) << 24) | (to_byte (r) << 16) | (to_byte (g) << 8) | to_byte (b);
}

Holly::Holly (Board *board_, hl::Buffer *vram_) :
	hl::Device (board_->machine),
	vram (vram_),
	board (board_)
{
	building = std::make_shared<Scene> ();
}

void
Holly::reset (hl::ResetType type)
{
	(void) type;
	memset (sb, 0, sizeof (sb));
	memset (pvr, 0, sizeof (pvr));
	memset (g1, 0, sizeof (g1));
	memset (g2, 0, sizeof (g2));
	memset (pd, 0, sizeof (pd));
	memset (maple, 0, sizeof (maple));
	memset (&ta, 0, sizeof (ta));
	memset (&yuv, 0, sizeof (yuv));
	memset (&mp, 0, sizeof (mp));
	sb[SB_SBREV] = 0x0B;
	ta.list = -1;
	delayed.clear ();
	contexts.clear ();
	draws.clear ();
	building = std::make_shared<Scene> ();
	frames = 0;
	line_now = 0;
	irl_pins = -1;
	istnrm_b = 0;
}

unsigned
Holly::lines () const
{
	unsigned n = ((pvr[PVR_SPG_LOAD] >> 16) & 0x3FF) + 1;

	return n < 200 ? 532 : n;
}

void
Holly::update_irq ()
{
	Sh4 *sh = (Sh4 *) board->sh;
	int level = 0, pins;

	if (sb[SB_ISTERR])
		sb[SB_ISTNRM] |= IST_ERROR;
	else
		sb[SB_ISTNRM] &= ~IST_ERROR;
	if (sb[SB_ISTEXT])
		sb[SB_ISTNRM] |= IST_EXT;
	else
		sb[SB_ISTNRM] &= ~IST_EXT;

	/* The second CLX2's causes, its masks being the first's */
	uint32_t nrm = sb[SB_ISTNRM] | istnrm_b;

	if ((nrm & sb[SB_IML6NRM]) || (sb[SB_ISTEXT] & sb[SB_IML6EXT]) || (sb[SB_ISTERR] & sb[SB_IML6ERR]))
		level = 6;
	else if ((nrm & sb[SB_IML4NRM]) || (sb[SB_ISTEXT] & sb[SB_IML4EXT]) || (sb[SB_ISTERR] & sb[SB_IML4ERR]))
		level = 4;
	else if ((nrm & sb[SB_IML2NRM]) || (sb[SB_ISTEXT] & sb[SB_IML2EXT]) || (sb[SB_ISTERR] & sb[SB_IML2ERR]))
		level = 2;

	pins = level ? 15 - level : -1;
	if (pins == irl_pins)
		return;
	if (irl_pins >= 0)
		sh->set_irq_state (SH4_IESOURCE_IRQ0 + irl_pins, hl::IRQ_STATE_CLEAR, this);
	if (pins >= 0)
		sh->set_irq_state (SH4_IESOURCE_IRQ0 + pins, hl::IRQ_STATE_RAISED, this);
	irl_pins = pins;
}

void
Holly::raise_ist (uint32_t bits)
{
	sb[SB_ISTNRM] |= bits;
	/* What both CLX2 do at once, the NAOMI 2's rendering, tells both */
	if (board->is_naomi2)
		istnrm_b |= bits & IST_BOTH_CLX;
	update_irq ();
}

void
Holly::set_external (uint32_t bit, bool on)
{
	if (on)
		sb[SB_ISTEXT] |= bit;
	else
		sb[SB_ISTEXT] &= ~bit;
	update_irq ();
}

void
Holly::after (unsigned count, uint32_t ist)
{
	delayed.push_back ({ count, ist });
}

void
Holly::line (unsigned n)
{
	uint32_t vb = pvr[PVR_VBLANK_INT], hb = pvr[PVR_HBLANK_INT];
	unsigned mode = (hb >> 12) & 3, comp = hb & 0x3FF;
	size_t i, kept = 0;

	line_now = n;
	for (i = 0; i < delayed.size (); i++) {
		if (delayed[i].lines > 1) {
			delayed[i].lines--;
			delayed[kept++] = delayed[i];
		} else
			raise_ist (delayed[i].ist);
	}
	delayed.resize (kept);

	/* The IRL pins are levels: what a handler left pending comes again (the
	 * CPU forgets it once taken), as the NAOMI 2's Elan handlers count on and
	 * what ended while the SH-4 masked it (F355 Twin's G1 DMAs) */
	if (irl_pins >= 0)
		((Sh4 *) board->sh)->set_irq_state (SH4_IESOURCE_IRQ0 + irl_pins, hl::IRQ_STATE_RAISED, this);

	if (n == (vb & 0x3FF)) {
		frames++;
		board->vblank_due = true;
		raise_ist (IST_VBL_IN);
		/* The Maple's DMA, started by the blank */
		if ((maple[0x10 / 4] & 1) && (maple[0x14 / 4] & 1) && mp.state == MP_IDLE)
			maple_start ();
	}
	if (n == ((vb >> 16) & 0x3FF))
		raise_ist (IST_VBL_OUT);
	if ((mode == 0 && n == comp) || (mode == 1 && comp && (n % comp) == 0) || mode == 2)
		raise_ist (IST_HBL_IN);

	if (mp.timer > 0 && !--mp.timer) {
		if (mp.state == MP_WAIT_REPLY)
			mp.state = MP_TIMEOUT;
		else if (mp.state == MP_WAIT_NOP)
			mp.state = MP_SEND;
		else if (mp.state == MP_DONE) {
			mp.state = MP_IDLE;
			maple[0x18 / 4] = 0;
			raise_ist (IST_DMA_MAPLE);
		}
		if (mp.state != MP_IDLE)
			maple_step ();
	}
}

/*
 * The DMAs
 */

void
Holly::ch2_dma ()
{
	Sh4 *sh = (Sh4 *) board->sh;
	uint32_t src = sh->ddt_source (2) & 0x1FFFFFFF;
	uint32_t dst = (sb[SB_C2DSTAT] & 0x03FFFFE0) | 0x10000000;
	uint32_t len = sb[SB_C2DLEN] ? sb[SB_C2DLEN] : 0x1000000, i;

	for (i = 0; i + 4 <= len; i += 4) {
		uint32_t val = 0;

		board->mmap->get (4, src + i, &val);
		area4_put (4, dst + i, val);
	}
	sh->ddt_done (2, src + len);
	if (dst & 0x01000000)
		sb[SB_C2DSTAT] = dst + len;
	sb[SB_C2DLEN] = 0;
	sb[SB_C2DST] = 0;
	after (1, IST_DMA_CH2);
}

void
Holly::g1_dma ()
{
	uint32_t star = g1[0x04 / 4], len = (g1[0x08 / 4] + 30) & ~30u;
	uint32_t offs = star & (board->ram->size () - 1);
	/* Aimed elsewhere (0: Strike Fighter's), the data read goes nowhere */
	bool in_ram = (star & 0x1C000000) == 0x0C000000;

	if (in_ram && offs + len > board->ram->size ())
		len = board->ram->size () - offs;
	if (g1[0x0C / 4] & 1) {
		std::vector<uint8_t> nowhere (in_ram ? 0 : len);
		uint8_t *to = in_ram ? (uint8_t *) board->ram->ptr (offs) : nowhere.data ();

		if (board->has_comm && board->comm.dma_to_ram (to, len)) {
			/* From the communication board */
		} else if (!board->link || !board->link->dma (len, to))
			board->cart->dma (to, len);
		((Sh4 *) board->sh)->blocks.new_epoch ();
	} else if (board->has_comm && in_ram) {
		/* From the RAM to the communication board's */
		board->comm.dma_from_ram ((const uint8_t *) board->ram->ptr (offs), len);
	}
	after (1 + (len >> 16), IST_DMA_GDROM);
}

void
Holly::g2_dma (unsigned ch)
{
	uint32_t *r = &g2[ch * 0x20 / 4];
	uint32_t aica = r[0] & 0x1FFFFFFF, sys = r[1] & 0x1FFFFFFF, len = r[2] & 0x7FFFFFFF, i;
	bool to_ram = r[3] & 1;

	for (i = 0; i + 4 <= len; i += 4) {
		uint32_t val = 0;

		if (to_ram) {
			board->mmap->get (4, aica + i, &val);
			board->mmap->put (4, sys + i, val);
		} else {
			board->mmap->get (4, sys + i, &val);
			board->mmap->put (4, aica + i, val);
		}
	}
	if (r[2] >> 31)
		r[5] = 0;
	r[2] = 0;
	r[6] = 0;
	after (1, IST_DMA_AICA << ch);
}

void
Holly::pvr_dma ()
{
	uint32_t vaddr = pd[0x00 / 4] & 0x1FFFFFFF, sys = pd[0x04 / 4] & 0x1FFFFFFF;
	uint32_t len = pd[0x08 / 4] ? pd[0x08 / 4] : 0x2000000, i;
	bool to_ram = pd[0x0C / 4] & 1;

	for (i = 0; i + 4 <= len; i += 4) {
		uint32_t val = 0;

		if (to_ram) {
			board->mmap->get (4, vaddr + i, &val);
			board->mmap->put (4, sys + i, val);
		} else {
			board->mmap->get (4, sys + i, &val);
			board->mmap->put (4, vaddr + i, val);
		}
	}
	pd[0x18 / 4] = 0;
	after (1, IST_DMA_PVR);
}

/*
 * The Maple Bus
 */

void
Holly::maple_start ()
{
	mp.address = maple[0x04 / 4];
	mp.state = MP_SEND;
	maple_step ();
}

void
Holly::maple_step ()
{
	hl::Mmap *mem = board->mmap;

	for (;;) {
		switch (mp.state) {
		case MP_SEND: {
			uint32_t header[2], data[256], length, pattern, i;

			mem->get (4, mp.address, &header[0]);
			mem->get (4, mp.address + 4, &header[1]);
			mp.address += 8;
			mp.end = header[0] >> 31;
			mp.port = (header[0] >> 16) & 3;
			pattern = (header[0] >> 8) & 7;
			length = (header[0] & 255) + 1;
			mp.dest = header[1];
			for (i = 0; i < length; i++)
				mem->get (4, mp.address + i * 4, &data[i]);
			mp.address += length * 4;

			switch (pattern) {
			case 0:
				if (board->is_atomiswave) {
					/* The controllers answer at once */
					uint32_t reply[64];
					unsigned words = board->controllers.reply (mp.port, data, reply);

					if (!words)
						mp.state = MP_TIMEOUT;
					else
						for (i = 0; i < words; i++)
							mem->put (4, mp.dest + i * 4, reply[i]);
					break;
				}
				/* The MIE alone, on port 0 */
				if (mp.port == 0) {
					board->mie->maple_write (data, length);
					mp.state = MP_WAIT_REPLY;
					mp.timer = MAPLE_TIMEOUT_LINES;
				} else
					mp.state = MP_TIMEOUT;
				break;
			case 3:
				if (mp.port == 0 && board->mie)
					board->mie->maple_reset ();
				break;
			case 7:
				mp.state = MP_WAIT_NOP;
				mp.timer = 1;
				break;
			default:
				break;
			}
			if (mp.state == MP_SEND && mp.end)
				mp.state = MP_DONE;
			break;
		}
		case MP_WAIT_REPLY:
		case MP_WAIT_NOP:
		case MP_IDLE:
			return;
		case MP_TIMEOUT:
			mem->put (4, mp.dest, 0xFFFFFFFF);
			mp.state = mp.end ? MP_DONE : MP_SEND;
			break;
		case MP_GOT_REPLY: {
			uint32_t data[16], length = 0, i;
			bool partial = false;

			mp.timer = 0;
			board->mie->maple_read (data, &length, &partial);
			for (i = 0; i < length; i++)
				mem->put (4, mp.dest + i * 4, data[i]);
			mp.dest += length * 4;
			if (partial) {
				mp.state = MP_WAIT_REPLY;
				mp.timer = MAPLE_TIMEOUT_LINES;
			} else
				mp.state = mp.end ? MP_DONE : MP_SEND;
			break;
		}
		case MP_DONE:
			mp.timer = MAPLE_DONE_LINES;
			return;
		}
	}
}

void
Holly::maple_end_of_reply ()
{
	if (mp.state == MP_WAIT_REPLY) {
		mp.state = MP_GOT_REPLY;
		maple_step ();
	}
}

/*
 * The Tile Accelerator
 */

Scene *
Holly::ta_scene ()
{
	return building.get ();
}

void
Holly::ta_reset_list ()
{
	ta.list = -1;
	ta.half = 0;
	ta.fifo_words = 0;
	ta.mod = 0;
	ta.sprite = 0;
	ta.strip_n = 0;
}

Batch &
Holly::ta_batch ()
{
	static const unsigned from[5] = { LIST_OPAQUE, LIST_OPAQUE, LIST_TRANSLUCENT, LIST_TRANSLUCENT, LIST_PUNCH };
	std::vector<Batch> &list = building->batches[from[ta.list & 7]];
	bool clip = (ta.clip_mode & 3) == 2;

	if (list.empty () || ta.new_batch) {
		Batch b = {};

		ta.new_batch = 0;
		b.isp = ta.isp;
		b.tsp = ta.tsp;
		b.tcw = ta.tcw;
		b.first = (uint32_t) building->vertices.size ();
		b.clip_on = clip;
		b.x0 = ta.clip[0];
		b.y0 = ta.clip[1];
		b.x1 = ta.clip[2];
		b.y1 = ta.clip[3];
		b.shadow = (ta.pcw >> 7) & 1;
		/* Glued to the last one if the same */
		if (!list.empty ()) {
			Batch &last = list.back ();
			if (last.isp == b.isp && last.tsp == b.tsp && last.tcw == b.tcw && last.clip_on == clip &&
			    last.x0 == b.x0 && last.y0 == b.y0 && last.x1 == b.x1 && last.y1 == b.y1 &&
			    last.shadow == b.shadow && last.first + last.count == b.first)
				return last;
		}
		list.push_back (b);
	}
	return list.back ();
}

void
Holly::ta_triangle (Vertex a, Vertex b, Vertex c)
{
	Scene *scene = building.get ();
	unsigned cull = (ta.isp >> 27) & 3;

	if (!((ta.isp >> 23) & 1)) {
		a.base = b.base = c.base;
		a.offset = b.offset = c.offset;
	}
	if (a.z <= 0.0f || b.z <= 0.0f || c.z <= 0.0f)
		return;
	if (cull) {
		float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
		float limit = to_float (pvr[PVR_CULL_VAL]);

		if (fabsf (area) < limit)
			return;
		if (cull == 2 && area < 0.0f)
			return;
		if (cull == 3 && area > 0.0f)
			return;
	}
	Batch &batch = ta_batch ();
	scene->vertices.push_back (a);
	scene->vertices.push_back (b);
	scene->vertices.push_back (c);
	scene->max_z = std::max ({ scene->max_z, a.z, b.z, c.z });
	batch.count += 3;
}

void
Holly::ta_store (Vertex v, bool end)
{
	if (ta.strip_n >= 2) {
		if ((ta.strip_n - 2) & 1) {
			/* Flat shading: the colour of the triangle's last vertex
			 * sent, which this order puts second */
			Vertex last = ta.strip[1];

			if (!((ta.isp >> 23) & 1)) {
				last.base = v.base;
				last.offset = v.offset;
			}
			ta_triangle (ta.strip[0], v, last);
		} else
			ta_triangle (ta.strip[0], ta.strip[1], v);
	}
	if (ta.strip_n >= 1)
		ta.strip[0] = ta.strip[1];
	ta.strip[ta.strip_n >= 1 ? 1 : 0] = v;
	if (ta.strip_n == 0)
		ta.strip[1] = v;
	ta.strip_n++;
	if (end)
		ta.strip_n = 0;
}

void
Holly::ta_polygon_header (const uint32_t *w)
{
	uint32_t pcw = w[0];
	unsigned col = (pcw >> 4) & 3;
	bool textured = (pcw >> 3) & 1, offset = (pcw >> 2) & 1, gouraud = (pcw >> 1) & 1, uv16 = pcw & 1;
	bool volume = (pcw >> 6) & 1;

	ta.pcw = pcw;
	ta.isp = (w[1] & 0xFC3FFFFF) | ((textured ? 1u : 0u) << 25) | ((offset ? 1u : 0u) << 24) |
	         ((gouraud ? 1u : 0u) << 23) | ((uv16 ? 1u : 0u) << 22);
	ta.tsp = w[2];
	ta.tcw = w[3];
	ta.mod = 0;
	ta.sprite = 0;
	ta.strip_n = 0;
	ta.new_batch = 1;
	if (pcw & (1 << 23))
		ta.clip_mode = (pcw >> 16) & 3;

	if (col == 2 && !volume && !(textured && offset)) {
		/* Polygon type 1, 32 bytes: its face colour at 10-1C */
		ta.face[0] = to_float (w[4]);
		ta.face[1] = to_float (w[5]);
		ta.face[2] = to_float (w[6]);
		ta.face[3] = to_float (w[7]);
	} else if (col == 2 && !volume) {
		ta.face[0] = to_float (w[8]);
		ta.face[1] = to_float (w[9]);
		ta.face[2] = to_float (w[10]);
		ta.face[3] = to_float (w[11]);
		{
			ta.face_offset[0] = to_float (w[12]);
			ta.face_offset[1] = to_float (w[13]);
			ta.face_offset[2] = to_float (w[14]);
			ta.face_offset[3] = to_float (w[15]);
		}
	} else if (col == 2 && volume) {
		ta.face[0] = to_float (w[8]);
		ta.face[1] = to_float (w[9]);
		ta.face[2] = to_float (w[10]);
		ta.face[3] = to_float (w[11]);
	}

	if (!textured) {
		if (volume)
			ta.vertex_type = col >= 2 ? 10 : 9;
		else
			ta.vertex_type = col >= 2 ? 2 : col;
	} else if (volume) {
		ta.vertex_type = (col >= 2 ? 13 : 11) + (uv16 ? 1 : 0);
	} else {
		switch (col) {
		case 0: ta.vertex_type = uv16 ? 4 : 3; break;
		case 1: ta.vertex_type = uv16 ? 6 : 5; break;
		default: ta.vertex_type = uv16 ? 8 : 7; break;
		}
	}
}

void
Holly::ta_vertex (const uint32_t *w)
{
	bool end = (w[0] >> 28) & 1;
	bool uv16 = ta.vertex_type == 4 || ta.vertex_type == 6 || ta.vertex_type == 8 ||
	            ta.vertex_type == 12 || ta.vertex_type == 14;
	Vertex v = {};
	float intensity, offs;

	v.x = to_float (w[1]);
	v.y = to_float (w[2]);
	v.z = to_float (w[3]);
	if (ta.vertex_type >= 3) {
		if (uv16) {
			v.u = to_float (w[4] & 0xFFFF0000);
			v.v = to_float (w[4] << 16);
		} else {
			v.u = to_float (w[4]);
			v.v = to_float (w[5]);
		}
	}
	switch (ta.vertex_type) {
	case 0:
		v.base = w[6];
		break;
	case 1:
		v.base = argb_float (to_float (w[4]), to_float (w[5]), to_float (w[6]), to_float (w[7]));
		break;
	case 2:
		intensity = to_float (w[6]);
		v.base = argb_float (ta.face[0], ta.face[1] * intensity, ta.face[2] * intensity, ta.face[3] * intensity);
		break;
	case 3:
	case 4:
		v.base = w[6];
		v.offset = w[7];
		break;
	case 5:
	case 6:
		v.base = argb_float (to_float (w[8]), to_float (w[9]), to_float (w[10]), to_float (w[11]));
		v.offset = argb_float (to_float (w[12]), to_float (w[13]), to_float (w[14]), to_float (w[15]));
		break;
	case 7:
	case 8:
		intensity = to_float (w[6]);
		offs = to_float (w[7]);
		v.base = argb_float (ta.face[0], ta.face[1] * intensity, ta.face[2] * intensity, ta.face[3] * intensity);
		v.offset = argb_float (ta.face_offset[0], ta.face_offset[1] * offs, ta.face_offset[2] * offs, ta.face_offset[3] * offs);
		break;
	case 9:
		v.base = w[4];
		break;
	case 10:
		intensity = to_float (w[4]);
		v.base = argb_float (ta.face[0], ta.face[1] * intensity, ta.face[2] * intensity, ta.face[3] * intensity);
		break;
	case 11:
	case 12:
		v.base = w[6];
		v.offset = w[7];
		break;
	case 13:
	case 14:
		intensity = to_float (w[6]);
		v.base = argb_float (ta.face[0], ta.face[1] * intensity, ta.face[2] * intensity, ta.face[3] * intensity);
		break;
	default:
		break;
	}
	ta_store (v, end);
}

void
Holly::ta_sprite (const uint32_t *w)
{
	Vertex a = {}, b = {}, c = {}, d = {};
	bool textured = (ta.isp >> 25) & 1;

	a.x = to_float (w[1]);  a.y = to_float (w[2]);  a.z = to_float (w[3]);
	b.x = to_float (w[4]);  b.y = to_float (w[5]);  b.z = to_float (w[6]);
	c.x = to_float (w[7]);  c.y = to_float (w[8]);  c.z = to_float (w[9]);
	d.x = to_float (w[10]); d.y = to_float (w[11]);
	/* D's depth on the plane of the others */
	{
		float e1x = b.x - a.x, e1y = b.y - a.y, e1z = b.z - a.z;
		float e2x = c.x - a.x, e2y = c.y - a.y, e2z = c.z - a.z;
		float k = e1x * e2y - e1y * e2x;

		d.z = a.z;
		if (k != 0.0f)
			d.z = a.z + ((e1y * e2z - e1z * e2y) * (a.x - d.x) + (e1z * e2x - e1x * e2z) * (a.y - d.y)) / k;
	}
	if (textured) {
		a.u = to_float (w[13] & 0xFFFF0000); a.v = to_float (w[13] << 16);
		b.u = to_float (w[14] & 0xFFFF0000); b.v = to_float (w[14] << 16);
		c.u = to_float (w[15] & 0xFFFF0000); c.v = to_float (w[15] << 16);
		d.u = a.u + c.u - b.u;
		d.v = a.v + c.v - b.v;
	}
	a.base = b.base = c.base = d.base = ta.sprite_base;
	a.offset = b.offset = c.offset = d.offset = ta.sprite_offset;
	{
		/* No culling, nor flat shading to change: the same colours */
		uint32_t isp = ta.isp;

		ta.isp |= 1 << 23;
		ta.isp &= ~(3u << 27);
		ta_triangle (a, b, c);
		ta_triangle (a, c, d);
		ta.isp = isp;
	}
}

void
Holly::ta_modifier (const uint32_t *w)
{
	Scene *scene = building.get ();
	int i;

	for (i = 1; i <= 9; i++)
		scene->mod_vertices.push_back (to_float (w[i]));
	/* The translucent lists' volumes are not drawn */
	if (ta.list == 1 && !scene->mod_volumes.empty ())
		scene->mod_volumes.back ().count += 3;
}

void
Holly::ta_packet (const uint32_t *w)
{
	uint32_t both[16];
	uint32_t pcw;
	unsigned para;

	if (ta.half) {
		memcpy (both, ta.fifo + 8, 32);
		memcpy (both + 8, w, 32);
		ta.half = 0;
		pcw = both[0];
		para = pcw >> 29;
		switch (para) {
		case 4:
			ta_polygon_header (both);
			break;
		default:
			if (ta.mod)
				ta_modifier (both);
			else if (ta.sprite)
				ta_sprite (both);
			else
				ta_vertex (both);
			break;
		}
		return;
	}

	pcw = w[0];
	para = pcw >> 29;
	switch (para) {
	case 0: {
		static const uint32_t ist[5] = { IST_EOXFER_OP, IST_EOXFER_OPMV, IST_EOXFER_TR, IST_EOXFER_TRMV, IST_EOXFER_PT };

		/* After the DMA that brought it says it is done (Kamui's list
		 * handler goes by that order) */
		if (ta.list >= 0 && ta.list < 5 && !from_elan)
			after (2, ist[ta.list]);
		ta.list = -1;
		ta.strip_n = 0;
		break;
	}
	case 1:
		ta.clip[0] = (uint16_t) (w[4] * 32);
		ta.clip[1] = (uint16_t) (w[5] * 32);
		ta.clip[2] = (uint16_t) ((w[6] + 1) * 32);
		ta.clip[3] = (uint16_t) ((w[7] + 1) * 32);
		ta.new_batch = 1;
		break;
	case 4: {
		unsigned list;

		if (ta.list < 0)
			ta.list = (pcw >> 24) & 7;
		list = ta.list;
		if (list == 1 || list == 3) {
			/* A modifier volume's header */
			ModVol mv;

			ta.mod = 1;
			ta.sprite = 0;
			/* Bit 26: the volume's last triangles, from the PCW */
			mv.isp = (w[1] & ~(1u << 26)) | (((pcw >> 6) & 1) << 26);
			mv.first = (uint32_t) building->mod_vertices.size () / 3;
			mv.count = 0;
			if (list == 1)
				building->mod_volumes.push_back (mv);
			break;
		}
		if ((((pcw >> 4) & 3) == 2 && (((pcw >> 6) & 1) || ((pcw >> 3) & 1 && (pcw >> 2) & 1)))) {
			memcpy (ta.fifo + 8, w, 32);
			ta.half = 1;
			break;
		}
		ta_polygon_header (w);
		break;
	}
	case 5: {
		bool textured = (pcw >> 3) & 1, offset = (pcw >> 2) & 1;

		if (ta.list < 0)
			ta.list = (pcw >> 24) & 7;
		ta.pcw = pcw;
		ta.isp = (w[1] & 0xFC3FFFFF) | ((textured ? 1u : 0u) << 25) | ((offset ? 1u : 0u) << 24) | (1u << 22);
		ta.tsp = w[2];
		ta.tcw = w[3];
		ta.sprite_base = w[4];
		ta.sprite_offset = w[5];
		ta.sprite = 1;
		ta.mod = 0;
		ta.new_batch = 1;
		if (pcw & (1 << 23))
			ta.clip_mode = (pcw >> 16) & 3;
		break;
	}
	case 7: {
		unsigned t = ta.vertex_type;

		if (ta.mod || ta.sprite || t == 5 || t == 6 || (t >= 11 && t <= 14)) {
			memcpy (ta.fifo + 8, w, 32);
			ta.half = 1;
			break;
		}
		ta_vertex (w);
		break;
	}
	default:
		break;
	}
}

/*
 * The Elan's Way In
 *
 * Its lists come in as the screen's triangles, through the same lists and
 * batches as the TA's. Its ends of list are told by its waits alone.
 */

void
Holly::elan_packet (const uint32_t *w)
{
	from_elan = true;
	ta_packet (w);
	from_elan = false;
}

void
Holly::elan_polygon (unsigned list, uint32_t isp, uint32_t tsp, uint32_t tcw, bool shadow)
{
	if (ta.list < 0)
		ta.list = list < 5 ? (int8_t) list : 0;
	ta.pcw = (ta.pcw & ~0x80u) | (shadow ? 0x80u : 0u);
	ta.isp = isp;
	ta.tsp = tsp;
	ta.tcw = tcw;
	ta.mod = 0;
	ta.sprite = 0;
	ta.strip_n = 0;
	ta.new_batch = 1;
}

void
Holly::elan_triangle (const Vertex &a, const Vertex &b, const Vertex &c)
{
	ta_triangle (a, b, c);
}

void
Holly::elan_modifier (unsigned list, uint32_t isp)
{
	if (ta.list < 0)
		ta.list = list < 5 ? (int8_t) list : 0;
	ta.mod = 0;
	ta.sprite = 0;
	if (ta.list == 1) {
		ModVol mv;

		mv.isp = isp;
		mv.first = (uint32_t) building->mod_vertices.size () / 3;
		mv.count = 0;
		building->mod_volumes.push_back (mv);
	}
}

void
Holly::elan_mod_triangle (const float *xyz)
{
	/* The translucent lists' volumes are not drawn, as the TA's */
	if (ta.list != 1 || building->mod_volumes.empty ())
		return;
	building->mod_vertices.insert (building->mod_vertices.end (), xyz, xyz + 9);
	building->mod_volumes.back ().count += 3;
}

void
Holly::elan_clip_mode (unsigned mode)
{
	if (ta.clip_mode != mode) {
		ta.clip_mode = (uint8_t) mode;
		ta.new_batch = 1;
	}
}

void
Holly::elan_end_of_list (uint32_t bits)
{
	after (1, bits);
	pvr[PVR_TA_ITP] += 32;
}

void
Holly::fifo_word (uint32_t word)
{
	ta.fifo[ta.fifo_words++] = word;
	if (ta.fifo_words == 8) {
		ta.fifo_words = 0;
		ta_packet (ta.fifo);
	}
}

/* The converter: 16x16 pixels from 64 U, 64 V, then four 8x8 of Y, into UYVY */
void
Holly::yuv_block ()
{
	uint32_t ctrl = pvr[PVR_YUV_CTRL];
	uint32_t width = ((ctrl & 0x3F) + 1) * 16, height = (((ctrl >> 8) & 0x3F) + 1) * 16;
	uint32_t base = pvr[PVR_YUV_BASE] & VRAM_MASK;
	const uint8_t *data = (const uint8_t *) yuv.data;
	const uint8_t *u = data, *v = data + 64, *y = data + 128;
	unsigned px, py;

	for (py = 0; py < 16; py++) {
		for (px = 0; px < 16; px++) {
			unsigned block = (px >> 3) | ((py >> 3) << 1);
			uint8_t luma = y[block * 64 + (py & 7) * 8 + (px & 7)];
			uint8_t chroma = (px & 1) ? v[(py / 2) * 8 + px / 2] : u[(py / 2) * 8 + px / 2];
			uint32_t at = base + ((yuv.y + py) * width + yuv.x + px) * 2;

			if (at + 1 <= VRAM_MASK) {
				vram->put (1, at, chroma);
				vram->put (1, at + 1, luma);
			}
		}
	}
	yuv.x += 16;
	if (yuv.x >= width) {
		yuv.x = 0;
		yuv.y += 16;
		if (yuv.y >= height) {
			yuv.y = 0;
			pvr[PVR_YUV_CNT]++;
			after (1, IST_EOXFER_YUV);
		}
	}
}

void
Holly::yuv_data (const uint32_t *w, unsigned words)
{
	while (words--) {
		yuv.data[yuv.bytes / 4] = *w++;
		yuv.bytes += 4;
		if (yuv.bytes == 384) {
			yuv.bytes = 0;
			yuv_block ();
		}
	}
}

void
Holly::area4_put (unsigned size, uint32_t addr, uint64_t val)
{
	uint32_t offs = addr & 0x01FFFFFF;
	uint32_t words[2] = { (uint32_t) val, (uint32_t) (val >> 32) };

	if (offs < 0x00800000) {
		fifo_word (words[0]);
		if (size == 8)
			fifo_word (words[1]);
	} else if (offs < 0x01000000) {
		yuv_data (words, size == 8 ? 2 : 1);
	} else {
		bool map = (addr & 0x02000000) ? (sb[SB_LMMODE1] & 1) : (sb[SB_LMMODE0] & 1);
		uint32_t target = offs & VRAM_MASK;

		if (map) {
			vram->put (size > 4 ? 4 : size, map32 (target), words[0]);
			if (size == 8)
				vram->put (4, map32 (target + 4), words[1]);
		} else
			vram->put (size, target, val);
	}
}

/*
 * The Registers
 */

void
Holly::start_render ()
{
	uint32_t base = pvr[PVR_PARAM_BASE] & 0x00F00000;
	Context *found = NULL;

	after (RENDER_LINES, IST_EOR_ISP | IST_EOR_TSP | IST_EOR_VIDEO);

	for (Context &c : contexts)
		if (c.base == base)
			found = &c;
	if (!found || draws.size () >= 16)
		return;

	Draw d;
	unsigned i;

	d.scene = found->scene;
	d.fb_w_sof1 = pvr[PVR_FB_W_SOF1];
	d.fb_w_ctrl = pvr[PVR_FB_W_CTRL];
	d.fb_w_linestride = pvr[PVR_FB_W_STRIDE];
	d.fb_x_clip = pvr[PVR_FB_X_CLIP];
	d.fb_y_clip = pvr[PVR_FB_Y_CLIP];
	d.text_control = pvr[PVR_TEXT_CONTROL];
	d.pal_ctrl = pvr[PVR_PAL_CTRL];
	d.pt_alpha_ref = pvr[PVR_PT_ALPHA] & 255;
	d.shad_scale = pvr[PVR_SHAD_SCALE];
	d.background_z = to_float (pvr[PVR_BACKGND_D]);
	d.to_texture = (pvr[PVR_FB_W_SOF1] & 0x01000000) != 0;
	d.scale_x = (pvr[PVR_SCALER_CTL] & 0x10000) ? 0.5f : 1.0f;
	{
		uint32_t vscale = pvr[PVR_SCALER_CTL] & 0xFFFF;

		d.scale_y = vscale ? 1024.0f / (float) vscale : 1.0f;
	}
	d.fog_color = pvr[PVR_FOG_COL_RAM];
	d.fog_color_vertex = pvr[PVR_FOG_COL_VERT];
	d.fog_density = pvr[PVR_FOG_DENSITY] & 0xFFFF;
	d.clamp_max = pvr[PVR_CLAMP_MAX];
	d.clamp_min = pvr[PVR_CLAMP_MIN];
	for (i = 0; i < 128; i++)
		d.fog_table[i] = (uint16_t) pvr[PVR_FOG_TABLE + i];
	memcpy (d.palette, &pvr[PVR_PALETTE], sizeof (d.palette));

	/* Sorted by the chip, unless told the lists are */
	d.presort = pvr[PVR_FEED_CFG] & 1;
	if (pvr[PVR_PARAM_CFG] & (1 << 21)) {
		uint32_t control = (uint32_t) vram->get (4, map32 (pvr[PVR_REGION_BASE] & VRAM_MASK & ~3u));

		d.presort = (control >> 29) & 1;
	}

	/* The background: its words at the tag */
	{
		uint32_t tag = pvr[PVR_BACKGND_T];
		uint32_t at = base + ((tag >> 3) & 0x1FFFFF) * 4;
		uint32_t skip = (tag >> 24) & 7;
		bool shadow = (tag >> 27) & 1 && (pvr[PVR_SHAD_SCALE] & 0x100);
		auto word = [&] (uint32_t a) { return (uint32_t) vram->get (4, map32 (a & VRAM_MASK & ~3u)); };

		d.background_isp = word (at);
		d.background_tsp = word (at + 4);
		d.background_tcw = word (at + 8);
		at += shadow ? 20 : 12;
		at += (tag & 7) * (skip * (shadow ? 2 : 1) + 3) * 4;
		for (i = 0; i < 3; i++) {
			Vertex &v = d.background[i];
			uint32_t p = at;
			bool textured = (d.background_isp >> 25) & 1, uv16 = (d.background_isp >> 22) & 1;

			v.x = to_float (word (p));
			v.y = to_float (word (p + 4));
			v.z = to_float (word (p + 8));
			p += 12;
			v.u = v.v = 0.0f;
			if (textured) {
				if (uv16) {
					v.u = to_float (word (p) & 0xFFFF0000);
					v.v = to_float (word (p) << 16);
					p += 4;
				} else {
					v.u = to_float (word (p));
					v.v = to_float (word (p + 4));
					p += 8;
				}
			}
			v.base = word (p);
			v.offset = 0;
			at += (skip * (shadow ? 2 : 1) + 3) * 4;
		}
	}
	draws.push_back (std::move (d));
}

void
Holly::pvr_write (unsigned reg, uint32_t val, uint32_t old)
{
	(void) old;

	switch (reg) {
	case PVR_ID:
	case PVR_REVISION:
		pvr[reg] = old;
		break;
	case PVR_SOFTRESET:
		if (val & 1)
			ta_reset_list ();
		break;
	case PVR_STARTRENDER:
		start_render ();
		break;
	case PVR_LIST_INIT:
		if (val >> 31) {
			uint32_t base = pvr[PVR_TA_ISP_BASE] & 0x00F00000;
			Context *found = NULL;

			ta_reset_list ();
			memset (ta.clip, 0, sizeof (ta.clip));
			ta.clip_mode = 0;
			ta.new_batch = 1;
			pvr[PVR_TA_NEXT_OPB] = pvr[PVR_OPB_INIT];
			pvr[PVR_TA_ITP] = pvr[PVR_TA_ISP_BASE];
			building = std::make_shared<Scene> ();
			for (Context &c : contexts)
				if (c.base == base)
					found = &c;
			if (found)
				found->scene = building;
			else
				contexts.push_back ({ base, building });
		}
		pvr[reg] = 0;
		break;
	case PVR_LIST_CONT:
		if (val >> 31)
			ta_reset_list ();
		pvr[reg] = 0;
		break;
	case PVR_YUV_BASE:
		yuv.x = yuv.y = 0;
		yuv.bytes = 0;
		pvr[PVR_YUV_CNT] = 0;
		break;
	default:
		break;
	}
}

void
Holly::sb_write (unsigned reg, uint32_t val, uint32_t old)
{
	switch (reg) {
	case SB_C2DST:
		if (!(old & 1) && (val & 1))
			ch2_dma ();
		break;
	case SB_SDST:
		if (val & 1) {
			sb[SB_SDST] = 0;
			after (1, 1 << 20);
		}
		break;
	case SB_SFRES:
		if (val == 0x7611) {
			if (board->link)
				board->link->software_reset ();
			reset_requested = true;
		}
		break;
	case SB_ISTNRM:
		sb[SB_ISTNRM] = old & ~(val | 0xC0000000);
		update_irq ();
		break;
	case SB_ISTEXT:
		sb[SB_ISTEXT] = old;
		break;
	case SB_ISTERR:
		sb[SB_ISTERR] = old & ~val;
		update_irq ();
		break;
	case SB_IML2NRM: case SB_IML2EXT: case SB_IML2ERR:
	case SB_IML4NRM: case SB_IML4EXT: case SB_IML4ERR:
	case SB_IML6NRM: case SB_IML6EXT: case SB_IML6ERR:
		update_irq ();
		break;
	default:
		break;
	}
}

int
Holly::get (unsigned size, uint32_t addr, void *val)
{
	uint32_t offs = addr & 0x00FFFFFF, data = 0;

	if (offs >= 0x5F6800 && offs < 0x5F6A00) {
		data = sb[(offs - 0x5F6800) / 4];
		if (board->is_naomi2 && (addr & 0x02000000) && offs == 0x5F6900)
			data = istnrm_b | (data & 0xC0000000);
	} else if (offs >= 0x5F6C00 && offs < 0x5F6D00) {
		data = maple[(offs - 0x5F6C00) / 4];
		if (offs == 0x5F6C18)
			data = mp.state != MP_IDLE;
	} else if (offs >= 0x5F7000 && offs < 0x5F7100) {
		if (offs >= 0x5F7018 && offs < 0x5F702C && board->has_comm)
			data = board->comm.get (offs);
		else if (!board->link || !board->link->g1_get (offs, &data))
			data = board->cart->get (offs & 0xFF);
	} else if (offs >= 0x5F7400 && offs < 0x5F7500) {
		data = g1[(offs - 0x5F7400) / 4];
	} else if (offs >= 0x5F7800 && offs < 0x5F7900) {
		data = g2[(offs - 0x5F7800) / 4];
	} else if (offs >= 0x5F7C00 && offs < 0x5F7D00) {
		data = pd[(offs - 0x5F7C00) / 4];
	} else if (offs >= 0x5F8000 && offs < 0x5FA000) {
		unsigned reg = (offs - 0x5F8000) / 4;

		switch (reg) {
		case PVR_ID:
			data = 0x17FD11DB;
			break;
		case PVR_REVISION:
			data = 0x11;
			break;
		case PVR_SPG_STATUS: {
			uint32_t vb = pvr[PVR_VBLANK_INT];
			bool blank = line_now >= (vb & 0x3FF) || line_now < ((vb >> 16) & 0x3FF);

			data = line_now | (blank ? (1 << 13) : 0);
			break;
		}
		default:
			data = pvr[reg];
			break;
		}
	} else
		return -1;
	set_ptr (val, size, data);
	return 0;
}

int
Holly::put (unsigned size, uint32_t addr, uint64_t val_)
{
	uint32_t offs = addr & 0x00FFFFFF, val = (uint32_t) val_;

	(void) size;
	if (offs >= 0x5F6800 && offs < 0x5F6A00) {
		unsigned reg = (offs - 0x5F6800) / 4;
		uint32_t old = sb[reg];

		sb[reg] = val;
		if (board->is_naomi2 && offs >= 0x5F6900 && offs < 0x5F6940) {
			bool second = (addr & 0x0E000000) == 0x02000000, both = (addr & 0x08000000) != 0;

			/* The second CLX2's masks do not matter: the first's are
			 * taken for both, and 085F69xx writes both */
			if (second && offs >= 0x5F6910) {
				sb[reg] = old;
				return 0;
			}
			if (offs == 0x5F6900 && (second || both)) {
				istnrm_b &= ~val;
				if (second) {
					sb[reg] = old;
					update_irq ();
					return 0;
				}
			}
		}
		sb_write (reg, val, old);
	} else if (offs >= 0x5F6C00 && offs < 0x5F6D00) {
		unsigned reg = (offs - 0x5F6C00) / 4;
		uint32_t old = maple[reg];

		maple[reg] = val;
		if (offs == 0x5F6C18 && !(old & 1) && (val & 1) && (maple[0x14 / 4] & 1) && !(maple[0x10 / 4] & 1))
			maple_start ();
	} else if (offs >= 0x5F7000 && offs < 0x5F7100) {
		if (offs >= 0x5F7018 && offs < 0x5F702C && board->has_comm)
			board->comm.put (offs, val);
		else if (!board->link || !board->link->g1_put (offs, val))
			board->cart->put (offs & 0xFF, val);
	} else if (offs >= 0x5F7400 && offs < 0x5F7500) {
		unsigned reg = (offs - 0x5F7400) / 4;
		uint32_t old = g1[reg];

		g1[reg] = val;
		if (offs == 0x5F7418 && !(old & 1) && (val & 1) && (g1[0x14 / 4] & 1)) {
			g1_dma ();
			g1[reg] = 0;
		}
	} else if (offs >= 0x5F7800 && offs < 0x5F7900) {
		unsigned reg = (offs - 0x5F7800) / 4;

		g2[reg] = val;
		if (reg < 32 && (reg & 7) == 6 && (val & 1) && (g2[reg - 1] & 1))
			g2_dma (reg / 8);
	} else if (offs >= 0x5F7C00 && offs < 0x5F7D00) {
		unsigned reg = (offs - 0x5F7C00) / 4;

		pd[reg] = val;
		if (offs == 0x5F7C18 && (val & 1) && (pd[0x14 / 4] & 1))
			pvr_dma ();
	} else if (offs >= 0x5F8000 && offs < 0x5FA000) {
		unsigned reg = (offs - 0x5F8000) / 4;
		uint32_t old = pvr[reg];

		pvr[reg] = val;
		pvr_write (reg, val, old);
	} else
		return -1;
	return 0;
}

std::vector<Draw>
Holly::take_draws ()
{
	std::vector<Draw> out;

	out.swap (draws);
	return out;
}

const char *
Holly::debug_string ()
{
	snprintf (debug_text, sizeof (debug_text), "HOLLY line %u frames %u ist %08X/%08X maple %u t %d port %u @%08X",
	          line_now, frames, sb[SB_ISTNRM], sb[SB_ISTEXT], mp.state, mp.timer, mp.port, mp.address);
	return debug_text;
}

#define SAVE(thing_) \
		ret |= state->put ((void *) &(thing_), sizeof (thing_))

#define LOAD(thing_) \
		ret |= state->get ((void *) &(thing_), sizeof (thing_))

int
Holly::save_state (hl::State *state)
{
	int ret = 0;

	SAVE (ta);
	SAVE (yuv);
	SAVE (sb);
	SAVE (pvr);
	SAVE (g1);
	SAVE (g2);
	SAVE (pd);
	SAVE (maple);
	SAVE (mp);
	SAVE (istnrm_b);
	SAVE (line_now);
	SAVE (frames);
	return ret;
}

int
Holly::load_state (hl::State *state)
{
	int ret = 0;

	LOAD (ta);
	LOAD (yuv);
	LOAD (sb);
	LOAD (pvr);
	LOAD (g1);
	LOAD (g2);
	LOAD (pd);
	LOAD (maple);
	LOAD (mp);
	LOAD (istnrm_b);
	LOAD (line_now);
	LOAD (frames);
	delayed.clear ();
	contexts.clear ();
	building = std::make_shared<Scene> ();
	irl_pins = -2;
	update_irq ();
	return ret;
}

#undef SAVE
#undef LOAD

} /* namespace naomi */
