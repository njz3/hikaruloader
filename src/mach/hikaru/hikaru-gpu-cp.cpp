/* 
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2011-2013, Stefano Teso
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

#include "hl/config.h"
#include "mach/hikaru/hikaru-gpu.h"
#include "mach/hikaru/hikaru-gpu-private.h"
#include "mach/hikaru/hikaru-renderer-private.h"
#include "mach/hikaru/hikaru.h"

namespace hikaru {

/*
 * GPU CP
 * ======
 *
 * The GPU CP is the Command Processor that performs 3D drawing.
 *
 *
 * CP Execution
 * ============
 *
 * CP execution is likely initiated by writing 15000058=3 and 1A000024=1, but
 * may actually begin only on the next vblank-in event.
 *
 * (From a software perspective, the CP program is uploaded when both IRQs 2 of
 * GPU 15 and GPU 1A are fired, meaning that at this point the CP is supposed
 * to have consumed the previous command stream and is ready to accept a new
 * one.)
 *
 * Note also that a CP program is uploaded to two different areas in CMDRAM on
 * odd and even frames. This may mean that there are *two* CPs performing
 * double-buffered 3D rendering.
 *
 * When execution ends:
 *
 *  - 1A00000C bit 0 is set (not sure)
 *    - 1A000018 bit 1 is set as a consequence
 *      - 15000088 bit 7 is set as a consequence
 *  - 15000088 bit 1 is set; a GPU IRQ is raised (if not masked by 15000084)
 *  - 15002000 bit 0 is set on some HW revisions (not sure)
 *  - 1A000024 bit 0 is cleared if some additional condition occurred (not sure)
 *
 * 15002000 and 1A000024 signal different things; see the termination condition
 * in sync().
 *
 * (Guesstimate: 15000088 bit 1 is set when the CP ends verifying/processing
 * the CS (if there is such a thing!); 15002000 is cleared when the CP submits
 * the completely rasterized frame buffer to the GPU 1A; 1A000024 is cleared
 * when the GPU 1A is done compositing the frame buffer with the 2D layers and
 * has displayed them on-screen.)
 */

static void
on_frame_begin (Gpu *gpu)
{
	if (gpu->debug.log_cp)
		HL_LOG (" ==== CLEARING CP DATA ==== ");

	/* A list still running at the vblank (BRAVEFF's demo, frames 7469 and
	 * 7476): its open mesh goes to this frame, the rest to the next one;
	 * the renderer would otherwise begin a mesh over it. The list keeps its
	 * state: cleared, its next static vertex had no precision. Both stopped
	 * the machine */
	if (gpu->state.in_mesh) {
		((Renderer *) gpu->renderer)->end_mesh (PC);
		gpu->state.in_mesh = 0;
	}
	if (gpu->cp.is_running)
		return;

	memset ((void *) &POLY, 0, sizeof (POLY));

	LOD.value = 0.0f;
	LOD.cond = false;
	LOD.branch_id = ~0;
	LOD.distance = 0.0f;
	LOD.size = 0.0f;

	VP.depth = 0;
	VP.scratch.flags = 0;

	MV.depth = 0;
	MV.total = 0;

	MAT.base = 0;
	MAT.scratch.flags = 0;

	TEX.base = 0;
	TEX.scratch.flags = 0;

	LIT.base = 0;
	LIT.scratch.flags = 0;
	LIT.scratchset.flags = 0;
}

static void
on_cp_begin (Gpu *gpu)
{
	if (gpu->debug.log_cp)
		HL_LOG (" ==== CP BEGIN ==== ");

	gpu->cp.is_running = true;
	gpu->lists_begun++;

	PC = REG15 (0x70);
	SP(0) = REG15 (0x74);
	SP(1) = REG15 (0x78);
	gpu->skin.sp = 0;
	gpu->link.valid = gpu->link.has_pushed = false;
	gpu->patterns = 0;
}

static void
on_cp_end (Gpu *gpu)
{
	if (gpu->debug.log_cp)
		HL_LOG (" ==== CP END ==== ");

	/* Turn off the busy bits */
	REG15 (0x58) &= ~3;
	REG1A (0x24) &= ~1;

	/* Notify that the GPUs are done and need feeding */
	hikaru_gpu_raise_irq (gpu, GPU15_IRQ_CMD_ANALYSIS_END, GPU1A_IRQ_PLOT_END);
}

void
hikaru_gpu_cp_vblank_in (Gpu *gpu)
{
	on_frame_begin (gpu);

	/* Notify that the drawing is done */
	hikaru_gpu_raise_irq (gpu, GPU15_IRQ_POLY_OUT_END, 0);
}

void
hikaru_gpu_cp_vblank_out (Gpu *gpu)
{
	/* no-op */
}

void
hikaru_gpu_cp_on_put (Gpu *gpu)
{
	/* Check the GPU 15 execute bits */
	if (REG15 (0x58) == 3)
		on_cp_begin (gpu);
	else
		REG1A (0x24) = 0; /* XXX really? */
}

/*
 * CP Objects
 * ==========
 *
 * The CP manipulates six kinds of objects: viewports, modelviews, materials,
 * textures/texheads (that's what they are called in PHARRIER, and here),
 * lights/lightsets (a lightset is a set of four lights), and meshes.
 *
 * The CP has a table for each object type, except meshes, namely:
 *
 *  viewports	8
 *  modelviews  < 256
 *  materials	16384 total? (can lookup at distance 0-255 from the base)
 *  texheads	16384 total? (can lookup at distance 0-255 from the base)
 *  lights	1024
 *  lightsets	256
 *
 * CP instructions do not manipulate the objects in the tables directly.
 * Instead, they work on a special object, the "scratch" or "active" object:
 * "recall" instructions load an object from the object table into the scratch
 * object, "set" instructions set the properties of the scratch objects, and
 * "commit" instructions store the scratch object back into the table.
 *
 * When a mesh is drawn, the current scratch objects affect its rendering,
 * e.g., the scratch material determines the mesh colors, shininess, etc.
 *
 *
 * CP Instructions
 * ===============
 *
 * Each GPU instruction is made of 1, 2, 4, or 8 32-bit words. The opcode is
 * specified by the lower 9 bits of the first word. The instruction size is
 * stored in bits 4-5 of the first word.
 */

#define HR        gpu->renderer

#define FLAG_JUMP	(1 << 0)
#define FLAG_BEGIN	(1 << 1)
#define FLAG_CONTINUE	(1 << 2)
#define FLAG_PUSH	(FLAG_BEGIN | FLAG_CONTINUE)
#define FLAG_STATIC	(1 << 3)
#define FLAG_INVALID	(1 << 4)

static struct {
	void (* handler)(Gpu *, uint32_t *);
	uint32_t flags;
} insns[0x200];

void (* disasm[0x200])(Gpu *, uint32_t *);

static uint32_t
get_insn_size (uint32_t *inst)
{
	return 1 << (((inst[0] >> 4) & 3) + 2);
}

static void
print_disasm (Gpu *gpu, uint32_t *inst, const char *fmt, ...)
{
	char out[256], *tmp = out;
	uint32_t nwords;
	va_list args;
	unsigned i;

	nwords = get_insn_size (inst) / 4;

	HL_ASSERT (gpu);
	HL_ASSERT (inst);
	HL_ASSERT (nwords <= 8);

	if (!gpu->debug.log_cp)
		return;

	va_start (args, fmt);
	tmp += sprintf (tmp, "CP @%08X : ", PC);
	for (i = 0; i < 8; i++) {
		if (i < nwords)
			tmp += sprintf (tmp, "%08X ", inst[i]);
		else
			tmp += sprintf (tmp, "........ ");
	}
	tmp += sprintf (tmp, "%c %c ",
	                UNHANDLED ? '!' : ' ',
	                gpu->state.in_mesh ? 'M' : ' ');
	vsnprintf (tmp, sizeof (out), fmt, args);
	va_end (args);

	HL_LOG ("%s", out);
}

#define DISASM(fmt_, ...) \
	print_disasm (gpu, inst, fmt_, ##__VA_ARGS__)

static void
check_self_loop (Gpu *gpu, uint32_t target)
{
	/* XXX at some point we'll need something better than this */
	if (target == PC) {
		HL_ERROR ("CP: @%08X: self-jump, terminating", target);
		gpu->cp.is_running = false;
	}
}

static void
push_pc (Gpu *gpu)
{
	HL_ASSERT ((SP(0) >> 24) == 0x48);
	gpu->cmdram->put (4, SP(0) & 0x3FFFFFF, PC);
	SP(0) -= 4;
}

static void
pop_pc (Gpu *gpu)
{
	SP(0) += 4;
	HL_ASSERT ((SP(0) >> 24) == 0x48);
	PC = gpu->cmdram->get (4, SP(0) & 0x3FFFFFF) + 8;
}

static int
fetch (Gpu *gpu, uint32_t **inst)
{
	Hikaru *hikaru = (Hikaru *) gpu->mach;

	/* The CP program has been observed to lie only in CMDRAM and slave
	 * RAM so far. */
	switch (PC >> 24) {
	case 0x40:
	case 0x41:
		*inst = (uint32_t *) hikaru->ram_s->ptr (PC & 0x01FFFFFF);
		return 0;
	case 0x48:
	case 0x4C: /* XXX not sure */
		*inst = (uint32_t *) hikaru->cmdram->ptr (PC & 0x003FFFFF);
		return 0;
	}
	return -1;
}

void
hikaru_gpu_cp_exec (Gpu *gpu, int cycles)
{
	if (!gpu->cp.is_running)
		return;

	while (cycles > 0 && gpu->cp.is_running) {
		uint32_t *inst, op;
		uint16_t flags;

		if (fetch (gpu, &inst)) {
			HL_ERROR ("CP %08X: invalid PC, skipping CS", PC);
			gpu->cp.is_running = false;
			break;
		}

		op = inst[0] & 0x1FF;

		flags = insns[op].flags;
		if (flags & FLAG_INVALID) {
			HL_ERROR ("CP @%08X: invalid instruction [%08X]", PC, *inst);
			gpu->cp.is_running = false;
			break;
		}

		if (!gpu->state.in_mesh && (flags & FLAG_BEGIN)) {
			bool is_static = (flags & FLAG_STATIC) != 0;
			((Renderer *) HR)->begin_mesh (PC, is_static);
			gpu->state.in_mesh = 1;
		} else if (gpu->state.in_mesh && !(flags & FLAG_CONTINUE)) {
			((Renderer *) HR)->end_mesh (PC);
			gpu->state.in_mesh = 0;
		}

		if (gpu->debug.log_cp) {
			UNHANDLED = 0;
			disasm[op] (gpu, inst);
			if (UNHANDLED)
				HL_ERROR ("CP @%08X : unhandled instruction", PC);
		}

		insns[op].handler (gpu, inst);

		if (!(flags & FLAG_JUMP))
			PC += get_insn_size (inst);

		cycles--;
	}

	if (!gpu->cp.is_running)
		on_cp_end (gpu);
}

#define I(name_) \
	static void hikaru_gpu_inst_##name_ (Gpu *gpu, uint32_t *inst)

#define D(name_) \
	static void hikaru_gpu_disasm_##name_ (Gpu *gpu, uint32_t *inst)

/*
 * Control Flow
 * ============
 *
 * The CP supports jumps and subroutine calls, including conditional calls
 * (for selecting the right mesh LOD probably?)
 *
 * The call stack is probably held in CMDRAM at the addresses specified by
 * MMIOs 1500007{4,8}.
 */

static uint32_t
get_jump_address (Gpu *gpu, uint32_t *inst)
{
	uint32_t addr;

	addr = inst[1] * 4;
	if (inst[0] & 0x800)
		addr += PC;

	return addr;
}

/* 000	Nop
 *
 *	-------- -------- -------o oooooooo
 */

I (0x000)
{
}

D (0x000)
{
	DISASM ("nop");

	UNHANDLED |= (inst[0] != 0);
}

/* 012	Jump
 *
 *	IIIIIIII IIIIIIII CCCCR--o oooooooo
 *	AAAAAAAA AAAAAAAA AAAAAAAA AAAAAAAA
 *
 * I = Branch identifier? With the conditions (LOD), a size in pixels
 * C = Condition
 * R = Relative
 * A = Address or Offset in 32-bit words.
 *
 * The LOD jumps (COND, !COND) come in rows, the largest size first: a model
 * jumps to its finest mesh when it covers I pixels on the screen (its
 * diameter, from 055's threshold, its radius, and its distance), to the next
 * one at a smaller I, and to its coarsest otherwise. PHARRIER: 140 and A0 for
 * the scenery, A0 and 20 for the characters (their shoes' soles are on the
 * finer meshes only).
 */

/* The LOD condition of a jump: the size against I after a 055, else the
 * threshold's condition alone */
static bool
get_lod_cond (Gpu *gpu, uint32_t branch_id)
{
	if (branch_id && LOD.size > 0.0f)
		return LOD.size >= (float) branch_id;
	return LOD.cond;
}

I (0x012)
{
	uint32_t addr = get_jump_address (gpu, inst);
	uint32_t branch_id = inst[0] >> 16;
	bool jump;

	switch ((inst[0] >> 12) & 0xF) {
	case 0x0:
		jump = true;
		break;
	case 0x1:
		jump = LOD.branch_id != branch_id;
		break;
	case 0x5:
	case 0x7:
		jump = get_lod_cond (gpu, branch_id);
		break;
	case 0x6:
	case 0x9:
		jump = !get_lod_cond (gpu, branch_id);
		break;
	case 0xD:
		jump = LOD.branch_id == branch_id;
		break;
	default:
		jump = false;
		break;
	}

	check_self_loop (gpu, addr);
	if (jump)
		PC = addr;
	else
		PC += 8;
}

D (0x012)
{
	static const char *cond[16] = {
		"",		"NEQ BID",	"?2?",		"?3?",
		"?4",		"COND",		"!COND",	"COND",
		"?8?",		"!COND",	"?A?",		"?B?",
		"?C?",		"EQ BID",	"?E?",		"?F?"
	};
	uint32_t addr = get_jump_address (gpu, inst);

	UNHANDLED |= !!(inst[0] & 0x00000600);

	DISASM ("jump %s @%08X [BID=%04X]",
	        cond[(inst[0] >> 12) & 0xF], addr, inst[0] >> 16);
}

/*
 * Skinning
 * ========
 *
 * AIRTRIX's skater is a routine of meshes in the rest pose (a T-pose), posed
 * with the bones of a table in the area that 1500001C points to (48100000).
 * On the board the slave poses it, in the program the ROM board's 315-5881
 * decrypts (see hikaru_set_rombd_config), into the routine the CP then
 * calls; without the chip it runs another program, which leaves the rest
 * pose, and the CP poses it here. The master SH-4 fills the table each frame
 * (AT:@0C0431B0):
 *
 *  +000  2 or 3
 *  +004  The routine, as the master SH-4 sees it (B4102120 for 48102120)
 *  +008  28 bones, 40 bytes each: a 4x4 matrix, column-major, the bone in
 *        the model's space
 *  +708  Their rest poses, inverted, in the same order (zero for none)
 *  +E20  Read back by the master once the frame is drawn; unknown
 *
 * The routine has four copies (frame & 3), written once: we pose the
 * vertices as the CP draws them. The hands and the board are other meshes,
 * drawn with the skater's matrix times their bone, so a vertex of bone b
 * goes to bone[b] x rest[b] x v. Which bone, the routine does not say (the
 * slave's program knows): no bits of the vertices, nor anything else in
 * CMDRAM, tell. We give each
 * vertex the nearest bone segment of the rest pose, from a joint to its
 * child's; a bone's parent being the nearest one whose matrix carries its
 * rest joint onto its posed joint. A guess that poses the skater as the
 * game does.
 */

static void
get_matrix (Gpu *gpu, uint32_t offs, mtx4x4f_t m)
{
	unsigned i, j;

	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++) {
			uint32_t w = (uint32_t) gpu->cmdram->get (4, offs + (i * 4 + j) * 4);
			memcpy (&m[i][j], &w, 4);
		}
}

/* m[column][row], as the modelviews */
static void
mult_mtx4x4f (mtx4x4f_t res, mtx4x4f_t a, mtx4x4f_t b)
{
	unsigned i, j;

	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++)
			res[i][j] = a[0][j] * b[i][0] + a[1][j] * b[i][1] +
			            a[2][j] * b[i][2] + a[3][j] * b[i][3];
}

static void
transform_point (vec3f_t res, mtx4x4f_t m, const vec3f_t p)
{
	vec3f_t tmp;
	unsigned j;

	for (j = 0; j < 3; j++)
		tmp[j] = m[0][j] * p[0] + m[1][j] * p[1] + m[2][j] * p[2] + m[3][j];
	memcpy (res, tmp, sizeof (tmp));
}

static float
distance (const vec3f_t a, const vec3f_t b)
{
	float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
	return sqrtf (x * x + y * y + z * z);
}

static float
segment_distance (const vec3f_t p, const vec3f_t a, const vec3f_t b)
{
	float ab[3], ap[3], len, t;
	vec3f_t q;
	unsigned i;

	for (i = 0; i < 3; i++) {
		ab[i] = b[i] - a[i];
		ap[i] = p[i] - a[i];
	}
	len = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
	t = (len > 0.0f) ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / len : 0.0f;
	t = (t < 0.0f) ? 0.0f : (t > 1.0f) ? 1.0f : t;
	for (i = 0; i < 3; i++)
		q[i] = a[i] + t * ab[i];
	return distance (p, q);
}

/* Whether the CP calls the routine of the skinning table: then its bones */
static bool
begin_skinning (Gpu *gpu, uint32_t addr)
{
	uint32_t base = (REG15 (0x1C) << 2) & 0x3FFFFF;
	uint32_t mode = (uint32_t) gpu->cmdram->get (4, base);
	uint32_t routine = (uint32_t) gpu->cmdram->get (4, base + 4);
	mtx4x4f_t posed[NUM_SKIN_BONES], rest[NUM_SKIN_BONES];
	vec3f_t joint[NUM_SKIN_BONES];
	static const vec3f_t origin = { 0.0f, 0.0f, 0.0f };
	unsigned parent[NUM_SKIN_BONES];
	bool valid[NUM_SKIN_BONES];
	unsigned b, c, p;

	if ((mode != 2 && mode != 3) || ((routine ^ addr) & 0x3FFFFF) ||
	    gpu->skin.slave_poses)
		return false;

	for (b = 0; b < NUM_SKIN_BONES; b++) {
		get_matrix (gpu, base + 8 + b * 0x40, posed[b]);
		get_matrix (gpu, base + 8 + (NUM_SKIN_BONES + b) * 0x40, rest[b]);
		valid[b] = posed[b][3][3] == 1.0f && rest[b][3][3] == 1.0f;
		if (!valid[b])
			continue;
		mult_mtx4x4f (gpu->skin.bone[b], posed[b], rest[b]);
		/* The joint: the inverted rest pose's translation, undone */
		for (c = 0; c < 3; c++)
			joint[b][c] = -(rest[b][c][0] * rest[b][3][0] +
			                rest[b][c][1] * rest[b][3][1] +
			                rest[b][c][2] * rest[b][3][2]);
	}

	for (c = 0; c < NUM_SKIN_BONES; c++) {
		float best = 0.0f;

		parent[c] = NUM_SKIN_BONES;
		if (!valid[c])
			continue;
		for (p = 0; p < NUM_SKIN_BONES; p++) {
			float dist, err;
			vec3f_t q;

			if (p == c || !valid[p])
				continue;
			transform_point (q, gpu->skin.bone[p], joint[c]);
			err = distance (q, posed[c][3]);
			dist = distance (joint[c], joint[p]);
			if (err < 1e-3f * MAX2 (1.0f, dist) &&
			    (parent[c] == NUM_SKIN_BONES || dist < best)) {
				parent[c] = p;
				best = dist;
			}
		}
	}
	/* A bone that none carries is a root, at the model's origin; one away
	 * from it takes no vertex (AIRTRIX's bone 0: the root's matrix, its
	 * joint at the head, which would sink the cap into the body) */
	for (b = 0; b < NUM_SKIN_BONES; b++)
		valid[b] = valid[b] && (parent[b] != NUM_SKIN_BONES ||
		                        distance (joint[b], origin) < 1e-3f);

	gpu->skin.num_segs = 0;
	for (c = 0; c < NUM_SKIN_BONES; c++) {
		p = parent[c];
		if (!valid[c] || p == NUM_SKIN_BONES || !valid[p] ||
		    distance (joint[c], joint[p]) <= 1e-4f)
			continue;
		auto *seg = &gpu->skin.seg[gpu->skin.num_segs++];
		seg->bone = p;
		memcpy (seg->a, joint[p], sizeof (vec3f_t));
		memcpy (seg->b, joint[c], sizeof (vec3f_t));
	}
	for (b = 0; b < NUM_SKIN_BONES; b++) {
		if (!valid[b])
			continue;
		auto *seg = &gpu->skin.seg[gpu->skin.num_segs++];
		seg->bone = b;
		memcpy (seg->a, joint[b], sizeof (vec3f_t));
		memcpy (seg->b, joint[b], sizeof (vec3f_t));
	}
	return gpu->skin.num_segs != 0;
}

static void
skin_vertex (Gpu *gpu, hikaru_vertex_t *v, bool has_normal)
{
	float best = 0.0f;
	unsigned i, bone = NUM_SKIN_BONES;
	vec3f_t n;

	for (i = 0; i < gpu->skin.num_segs; i++) {
		auto *seg = &gpu->skin.seg[i];
		float dist = segment_distance (v->body.position, seg->a, seg->b);
		if (bone == NUM_SKIN_BONES || dist < best) {
			bone = seg->bone;
			best = dist;
		}
	}
	if (bone == NUM_SKIN_BONES)
		return;

	auto &m = gpu->skin.bone[bone];
	transform_point (v->body.position, m, v->body.position);
	if (has_normal) {
		for (i = 0; i < 3; i++)
			n[i] = m[0][i] * v->body.normal[0] +
			       m[1][i] * v->body.normal[1] +
			       m[2][i] * v->body.normal[2];
		memcpy (v->body.normal, n, sizeof (n));
	}
}

/* 052	Call
 *
 *	-------- -------- CCCCR--o oooooooo
 *	AAAAAAAA AAAAAAAA AAAAAAAA AAAAAAAA
 *
 * C = Condition
 * R = Relative
 * A = Address or Offset in 32-bit words.
 */

I (0x052)
{
	uint32_t addr = get_jump_address (gpu, inst);
	bool jump;

	switch ((inst[0] >> 12) & 0xF) {
	case 0x0:
		jump = true;
		break;
	case 0x4:
		jump = LOD.cond == false;
		break;
	case 0x8:
		jump = LOD.cond == true;
		break;
	default:
		jump = false;
		break;
	}

	check_self_loop (gpu, addr);
	if (jump) {
		push_pc (gpu);
		/* The caller's dot pattern, for the return (see 501) */
		gpu->patterns = ((gpu->patterns << 5) | (gpu->pattern & 0x1F)) & 0x3FFFFFFF;
		if (begin_skinning (gpu, addr))
			gpu->skin.sp = SP(0);
		PC = addr;
	} else
		PC += 8;
}

D (0x052)
{
	static const char *cond[16] = {
		"",		"?1?",		"?2?",		"?3?",
		"!COND",	"?5?",		"?6?",		"?7?",
		"COND",		"?9?",		"?A?",		"?B?",
		"?C?",		"?D?",		"?E?",		"?F?",
	};
	uint32_t addr = get_jump_address (gpu, inst);

	UNHANDLED |= !!(inst[0] & 0xFFFF3600);

	DISASM ("call %s @%08X", cond[(inst[0] >> 12) & 0xF], addr);
}

/* 082	Return
 *
 *	-------- -------- CCCC---o oooooooo
 *
 * C = Condition
 */

I (0x082)
{
	bool jump;

	switch ((inst[0] >> 12) & 0xF) {
	case 0x0:
		jump = true;
		break;
	case 0x4:
		jump = LOD.cond == false;
		break;
	case 0x8:
		jump = LOD.cond == true;
		break;
	default:
		jump = false;
		break;
	}

	if (jump) {
		pop_pc (gpu);
		gpu->pattern = gpu->patterns & 0x1F;
		gpu->patterns >>= 5;
		/* Out of the skinned routine */
		if (gpu->skin.sp && SP(0) > gpu->skin.sp)
			gpu->skin.sp = 0;
	} else
		PC += 4;
}

D (0x082)
{
	static const char *cond[16] = {
		"",		"?1?",		"?2?",		"?3?",
		"!COND",	"?5?",		"?6?",		"?7?",
		"COND",		"?9?",		"?A?",		"?B?",
		"?C?",		"?D?",		"?E?",		"?F?",
	};
	UNHANDLED |= !!(inst[0] & 0xFFFF3E00);

	DISASM ("ret %s", cond[(inst[0] >> 12) & 0xF]);
}

/* 1C2	Kill
 *
 *	-------- -------- -------o oooooooo
 */

I (0x1C2)
{
	gpu->cp.is_running = false;
}

D (0x1C2)
{
	UNHANDLED |= !!(inst[0] & 0xFFFFFE00);

	DISASM ("kill");
}

/* 005	LOD: Set Threshold Lower-Bound
 *
 *	TTTTTTTT TTTTTTTT TTTT---o oooooooo
 *
 * T = Truncated floating-point threshold.
 *
 *	Since the threshold is always positive, truncating the lower 12 bits
 *	makes the resulting value smaller = a lower bound.
 *
 * Used in conjunction with conditional control-flow. See SGNASCAR.
 */

I (0x005)
{
	alias32uf_t thresh;

	thresh.u = inst[0] & 0xFFFFF000;

	LOD.cond = LOD.value < (thresh.f * 8.0f);
	LOD.size = 0.0f;
}

D (0x005)
{
	alias32uf_t thresh;

	thresh.u = inst[0] & 0xFFFFF000;

	UNHANDLED |= !!(inst[0] & 0x00000E00);

	DISASM ("lod: set threshold lb [%f]", thresh.f);
}

/* 055	LOD: Set Threshold
 *
 *	-------- -------- -------o oooooooo
 *	TTTTTTTT TTTTTTTT TTTTTTTT TTTTTTTT
 *
 * T = Floating-point threshold: the model's radius (PHARRIER's characters'
 *     feet: 0.12)
 *
 * The model's size on the screen, its diameter seen from the distance of
 * the LOD vector (561) through the viewport's projection, goes to the jumps
 * that follow (012), in 1/16 of a pixel: 32 * radius * f / distance. As
 * Odin's author, Réda Chérif-Touil, found it: SGNASCAR draws each piece of
 * its scenery twice, near and far, and its sizes are those of 300, 400 and
 * 500 units in a view 60 degrees wide (f 554.256). Counted in 1/8 of a
 * pixel before (a guess from PHARRIER's characters, their feet 32 pixels
 * wide in the demo, who then took their finest, rounded shoes: they still
 * do).
 */

#define LOD_SCALE	16.0f

I (0x055)
{
	float thresh = *(float *) &inst[1];

	LOD.cond = LOD.value < (thresh * 4.0f);
	LOD.size = (LOD.distance > 0.0f) ?
	           LOD_SCALE * 2.0f * thresh * fabsf (VPC.clip.f) / LOD.distance : 0.0f;
}

D (0x055)
{
	UNHANDLED |= !!(inst[0] & 0xFFFFFE00);

	DISASM ("lod: set threshold [%f]", *(float *) &inst[1]);
}

/* 095	LOD: Set Branch IDs
 *
 *	-------- -------- CCCC---o oooooooo
 *	HHHHHHHH HHHHHHHH LLLLLLLL LLLLLLLL
 *
 * C = Condition
 * H, L = Branch IDs
 */

I (0x095)
{
	uint16_t hi = inst[1] >> 16;
	uint16_t lo = inst[1] & 0xFFFF;

	switch ((inst[0] >> 12) & 0xF) {
	case 0x4:
		LOD.branch_id = LOD.cond ? lo : hi;
		break;
	case 0x8:
		LOD.branch_id = LOD.cond ? hi : lo;
		break;
	default:
		LOD.branch_id = ~0;
		break;
	}
}

D (0x095)
{
	UNHANDLED |= !!(inst[0] & 0xFFFF3E00);

	DISASM ("lod: set branch ids [%04X %04X]",
	        inst[1] >> 16, inst[1] & 0xFFFF);
}

/*
 * Viewports
 * =========
 *
 * These specify an on-screen rectangle (a subregion of the framebuffer,
 * presumably), a projection matrix, the depth-buffer and depth queue
 * configuration, ambient lighting and clear color. The exact meaning of the
 * various fields are still partially unknown.
 */

static uint32_t
get_viewport_index (uint32_t *inst)
{
	return (inst[0] >> 16) & (NUM_VIEWPORTS - 1);
}

static float
decode_clip_xy (uint32_t c)
{
	return (float) ((((int32_t) (int16_t) c) << 3) >> 3);
}

/* 021	Viewport: Set Z Clipping
 *
 *	-------- -------- -------o oooooooo
 *	FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFF
 *	ffffffff ffffffff ffffffff ffffffff
 *	NNNNNNNN NNNNNNNN NNNNNNNN NNNNNNNN
 *
 * F = Far clipping plane
 * f = Alt. far clipping plane (see SGNASCAR)
 * N = Near clipping plane
 *
 * See PH:@0C01587C, PH:@0C0158A4, PH:@0C0158E8.
 *
 *
 * 221	Viewport: Set XY Clipping and Offset
 *
 *	-------- -------- -------o oooooooo
 *	YYYYYYYY YYYYYYYY XXXXXXXX XXXXXXXX
 *	--BBBBBB BBBBBBBB -LLLLLLL LLLLLLLL
 *	--TTTTTT TTTTTTTT -RRRRRRR RRRRRRRR
 *
 * T, B, L, R = Clipping planes
 * X, Y = Viewport offset
 *
 * See PH:@0C015924
 *
 *
 * 421	Viewport: Set Depth Range
 *
 *	-------- -------- -------o oooooooo
 *	mmmmmmmm mmmmmmmm mmmmmmmm mmmmmmmm
 *	MMMMMMMM MMMMMMMM MMMMMMMM MMMMMMMM
 *	FFF----- -------- -------- --------
 *
 * m = Minimum
 * M = Maximum
 * F = Depth test function
 *
 * See PH:@0C015AA6
 *
 *
 * 621	Viewport: Set Depth Queue
 *
 *	-------- ---TTTDW -------o oooooooo
 *	AAAAAAAA BBBBBBBB GGGGGGGG RRRRRRRR
 *	PPPPPPPP PPPPPPPP PPPPPPPP PPPPPPPP
 *	QQQQQQQQ QQQQQQQQ QQQQQQQQ QQQQQQQQ
 *
 * W = Disable depth writes
 * D = No colour: the polygons fade into what is behind them
 * T = Type: 0 linear; 2, 4 and 6 send a density as P (not understood)
 * R, G, B, A = Fog color (A unknown: 00 in SGNASCAR's and SWRACER's, FF in
 *     some of AIRTRIX's); not written with D
 * P = Fog density: 1/z where the fog starts; with D, 1 / (end - start)
 * Q = Fog offset: 1/z where it is whole; with D, near / start
 *
 * The fog is taken as linear in 1/z (as a rasterizer interpolates it):
 * SGNASCAR's from 5000 to 100000, AIRTRIX's halfpipe's from 10000 to
 * 140000, SWRACER's tracks' from 9000 to 1000000: little of it shows, as
 * in the attracts. The materials with 881's z take none (depth blend).
 *
 * With D, PHARRIER's library (0C015A3E, start and end; 0C015A72, type,
 * density and start) has the meshes that do not blend (opaque,
 * punch-through) fade into what is behind them, linearly in the eye's
 * depth from start to end; its viewport menu names the values "Depth
 * Queue Type", "Start", "End" and "Dns". As Odin's author, Réda
 * Chérif-Touil, found it (the far scenery of PHARRIER's demo play fades on
 * its cabinet, the translucent mountains stay). The X360 port fogs
 * linearly in the eye's depth too, from P and Q as the VON4 library
 * builds them: the coloured fog in 1/z here is still to compare.
 *
 * See PH:@0C0159C4, PH:@0C015A02, PH:@0C015A3E.
 */

I (0x021)
{
	hikaru_viewport_t *vp = &VP.scratch;

	switch ((inst[0] >> 8) & 7) {
	case 0:
		vp->clip.f = *(float *) &inst[1];
		vp->clip.f2 = *(float *) &inst[2];
		vp->clip.n = *(float *) &inst[3];
		vp->has_021 = 1;
		break;
	case 2:
		vp->offset.x = (float) (inst[1] & 0xFFFF);
		vp->offset.y = (float) (inst[1] >> 16);
		vp->clip.l = decode_clip_xy (inst[2]);
		vp->clip.r = decode_clip_xy (inst[3]);
		vp->clip.b = decode_clip_xy (inst[2] >> 16);
		vp->clip.t = decode_clip_xy (inst[3] >> 16);
		vp->has_221 = 1;
		break;
	case 4:
		vp->depth.min = *(float *) &inst[1];
		vp->depth.max = *(float *) &inst[2];
		vp->depth.func = inst[3] >> 29;
		vp->has_421 = 1;
		break;
	case 6:
		vp->depth.q_type	= (inst[0] >> 18) & 7;
		vp->depth.q_enabled	= ((inst[0] >> 17) & 1) ^ 1;
		vp->depth.q_unknown	= (inst[0] >> 16) & 1;
		vp->depth.mask[0]	= inst[1] & 0xFF;
		vp->depth.mask[1]	= (inst[1] >> 8) & 0xFF;
		vp->depth.mask[2]	= (inst[1] >> 16) & 0xFF;
		vp->depth.mask[3]	= inst[1] >> 24;
		vp->depth.density	= *(float *) &inst[2];
		vp->depth.bias		= *(float *) &inst[3];
		vp->has_621 = 1;
		break;
	default:
		HL_ASSERT (0);
		break;
	}
}

D (0x021)
{
	switch ((inst[0] >> 8) & 7) {
	case 0:
		UNHANDLED |= !!(inst[0] & 0xFFFFF800);

		DISASM ("vp: set clip Z [f=%f f2=%f n=%f]",
		        *(float *) &inst[1], *(float *) &inst[2], *(float *) &inst[3]);
		break;

	case 2:
		UNHANDLED |= !!(inst[0] & 0xFFFFF800);
	
		DISASM ("vp: set clip XY [clipxy=(%f %f %f %f) offs=(%f,%f)]",
		        decode_clip_xy (inst[2]), decode_clip_xy (inst[3]),
		        decode_clip_xy (inst[2] >> 16), decode_clip_xy (inst[3] >> 16),
		        (float) (inst[1] & 0xFFFF), (float) (inst[1] >> 16));
		break;

	case 4:
		UNHANDLED |= !!(inst[0] & 0xFFFFF800);
		UNHANDLED |= !!(inst[3] & 0x1FFFFFFF);

		DISASM ("vp: set depth [func=%u range=(%f,%f)]",
		        inst[3] >> 29, *(float *) &inst[1], *(float *) &inst[2]);
		break;

	case 6:
		UNHANDLED |= !!(inst[0] & 0xFFF0F800);
	
		DISASM ("vp: set depth queue [type=%u ena=%u unk=%u mask=(%08X) density=%f bias=%f]",
		        (inst[0] >> 18) & 3, ((inst[0] >> 17) & 1) ^ 1,
		        (inst[0] >> 16) & 1, inst[1],
		        *(float *) &inst[2], *(float *) &inst[3]);
		break;
	}
}

/* 011	Viewport: Set Ambient Color
 *
 *	rrrrrrrr rrrrrrrr ----1--o oooooooo
 *	bbbbbbbb bbbbbbbb gggggggg gggggggg
 *
 * r,g,b = color
 *
 * The ambient light, times each lit material's 691. A recall (003) keeps it:
 * AIRTRIX commits viewport 1 without one at the start of a frame and draws
 * its skater in it, lit by the ambient set last (B2B2B2, the frame before).
 * Restoring the viewport's left the skater black under its spots.
 *
 * See PH:@0C037840.
 */

I (0x011)
{
	hikaru_viewport_t *vp = &VP.scratch;

	vp->color.ambient[0] = inst[0] >> 16;
	vp->color.ambient[1] = inst[1] & 0xFFFF;
	vp->color.ambient[2] = inst[1] >> 16;

	vp->has_011 = 1;
}

D (0x011)
{
	UNHANDLED |= !!(inst[0] & 0x0000F600);
	UNHANDLED |= !(inst[0] & 0x00000800);

	DISASM ("vp: set ambient [%X %X %X]",
	        inst[0] >> 16, inst[1] & 0xFFFF, inst[1] >> 16);
}

/* 191	Viewport: Set Clear Color
 *
 *	-------- -------- ----1--o oooooooo
 *	-------a gggggggg bbbbbbbb rrrrrrrr
 *
 * a,r,g,b = color.
 *
 * NOTE: yes, apparently blue and green _are_ swapped.
 *
 * XXX double check the alpha mask.
 *
 * See PH:@0C016368, PH:@0C016396, PH:@0C037760.
 */

I (0x191)
{
	hikaru_viewport_t *vp = &VP.scratch;

	vp->color.clear[0] = inst[1] & 0xFF;
	vp->color.clear[1] = (inst[1] >> 8) & 0xFF;
	vp->color.clear[2] = (inst[1] >> 16) & 0xFF;
	vp->color.clear[3] = ((inst[1] >> 24) & 1) ? 0xFF : 0;

	gpu->fb_config.clear[0] = inst[1] & 0xFF;                   
	gpu->fb_config.clear[1] = (inst[1] >> 8) & 0xFF;            
	gpu->fb_config.clear[2] = (inst[1] >> 16) & 0xFF;           
	gpu->fb_config.clear[3] = ((inst[1] >> 24) & 1) ? 0xFF : 0; 

	vp->has_191 = 1;
}

D (0x191)
{
	UNHANDLED |= !!(inst[0] & 0xFFFFF600);
	UNHANDLED |= !(inst[0] & 0x00000800);
	UNHANDLED |= !!(inst[0] & 0xFE000000);

	DISASM ("vp: set clear [%X]", inst[1]);
}

/* 004	Commit Viewport
 *
 *	-------- -----iii -------o oooooooo
 *
 * i = Index
 *
 * See PH:@0C015AD0.
 */

I (0x004)
{
	hikaru_viewport_t *vp = &VP.table[get_viewport_index (inst)];

	*vp = VP.scratch;
}

D (0x004)
{
	UNHANDLED |= !!(inst[0] & 0xFFF8FE00);

	DISASM ("vp: commit @%u", get_viewport_index (inst));
}

/* 003	Recall Viewport
 *
 *	-------- -----iii -pP----o oooooooo
 *
 * i = Index
 *
 * P = Push
 *
 *	Pushes current viewport on the stack and uses the one specified by i.
 *
 * p = Pop
 *
 *	Pops the viewport on the stack and uses it.
 *
 * Information kindly provided by DreamZzz.
 *
 * The recall picks the viewport the polygons use (Gpu::vp_current) and
 * leaves the registers 021-991 write into alone, as Odin (Réda Chérif-Touil,
 * GPL v3+) has it: the viewport pushed is the one recalled before, not what
 * the registers hold since. hikcheck's two-monitor test recalls its text
 * viewport (640x480, f 1280), sets the registers to its 3D view, commits
 * that, pushes it and pops back to the text's: its "MONITOR #1" and "PRESS
 * TEST BUTTON TO EXIT" went through the 3D's projection, a third of their
 * size, lost in the middle of the picture.
 *
 * See PH:@0C015AF6, PH:@0C015B12, PH:@0C015B32.
 */

I (0x003)
{
	hikaru_viewport_t *vp = &VPC;

	switch ((inst[0] >> 12) & 0xF) {
	case 0:
		*vp = VP.table[get_viewport_index (inst)];
		break;
	case 2:
		VP.stack[VP.depth] = *vp;
		VP.depth++;
		HL_ASSERT (VP.depth < 32);

		*vp = VP.table[get_viewport_index (inst)];
		break;
	case 4:
		VP.depth--;
		HL_ASSERT (VP.depth >= 0);
		*vp = VP.stack[VP.depth];
		break;
	default:
		HL_ASSERT (0);
		break;
	}
}

D (0x003)
{
	const char *op;

	switch ((inst[0] >> 12) & 0xF) {
	case 0:
		op = "";
		break;
	case 2:
		op = "push";
		break;
	case 4:
		op = "pop";
		break;
	default:
		op = "unknown";
		UNHANDLED = 1;
		break;
	}

	UNHANDLED |= !!(inst[0] & 0xFFF89E00);

	DISASM ("vp: recall @%u %s", get_viewport_index (inst), op);
}

/*
 * Modelview Matrix
 * ================
 *
 * The CP uses command 161 to upload each (row) vector of the modelview matrix
 * separately.  The CP can also perform instanced drawing, and instructed to do
 * se through command 161.
 *
 * The other commands here set various vectors used for lighting (i.e. light
 * position and direction) but are not well understood.
 */

static void
mult_mtx4x4f_vec4f (vec4f_t res, mtx4x4f_t m, vec4f_t v)
{
	unsigned i;
	for (i = 0; i < 4; i++)
		res[i] = m[0][i] * v[0] + m[1][i] * v[1] + m[2][i] * v[2] + m[3][i] * v[3];
}

static float
norm_vec4 (vec4f_t v)
{
	return sqrtf (v[0]*v[0] + v[1]*v[1] + v[2]*v[2] + v[3]*v[3]);
}

/* The current matrix is set: the vertices with S take the one pushed before
 * it, if any (the renderer draws them so) */
static void
set_link (Gpu *gpu)
{
	gpu->link.valid = gpu->link.has_pushed;
	gpu->link.has_pushed = false;
}

/* 161	Set Matrix Vector
 *
 *	-------- ----MPNN --U----o oooooooo
 *	XXXXXXXX XXXXXXXX XXXXXXXX XXXXXXXX
 *	YYYYYYYY YYYYYYYY YYYYYYYY YYYYYYYY
 *	ZZZZZZZZ ZZZZZZZZ ZZZZZZZZ ZZZZZZZZ
 *
 * M = Multiply with previous?
 *
 * P = Push
 *
 *	Not the current matrix: one kept aside, which the vertices with S
 *	(see 12x) take, the other ones the current matrix; the renderer
 *	gives a mesh both. So a chain of parts joins with no gap: each part
 *	is drawn with its matrix after its parent's, pushed, its vertices at
 *	the joint with S (AIRTRIX's hands: they land on the parent's
 *	exactly). The matrices may be flat, shadows' projections. Not
 *	instanced drawing: no game draws a mesh once a matrix. AIRTRIX also
 *	pushes runs of 20-27, the last positions of the board, before the
 *	skater; unknown use.
 *
 * U = Unknown
 *
 * N = Column index
 *
 *
 * 561	LOD: Set Vector
 *
 *	-------- ------NN -WW----o oooooooo
 *	XXXXXXXX XXXXXXXX XXXXXXXX XXXXXXXX
 *	YYYYYYYY YYYYYYYY YYYYYYYY YYYYYYYY
 *	ZZZZZZZZ ZZZZZZZZ ZZZZZZZZ ZZZZZZZZ
 *
 * N = Always 11b
 *
 * W = Always 11b
 *
 * Uploads a vector used for LOD computations.
 *
 *
 * 961	Light: Set Vector 9
 *
 *	-------- -------T TWW----o oooooooo
 *	XXXXXXXX XXXXXXXX XXXXXXXX XXXXXXXX
 *	YYYYYYYY YYYYYYYY YYYYYYYY YYYYYYYY
 *	ZZZZZZZZ ZZZZZZZZ ZZZZZZZZ ZZZZZZZZ
 *
 * T = Direction/Position/etc.
 * W = Unknown
 *
 *
 * B61	Light: Set Vector B
 *
 *	-------- -------- TWW----o oooooooo
 *	XXXXXXXX XXXXXXXX XXXXXXXX XXXXXXXX
 *	YYYYYYYY YYYYYYYY YYYYYYYY YYYYYYYY
 *	ZZZZZZZZ ZZZZZZZZ ZZZZZZZZ ZZZZZZZZ
 *
 * T = Direction/Position/etc.
 * W = Unknown
 */

I (0x161)
{
	hikaru_modelview_t *mv = &MV.table[MV.depth];
	hikaru_light_t *lit = &LIT.scratch;
	uint32_t push, elem;

	switch ((inst[0] >> 8) & 0xF) {

	case 0x1:
		/* Bits 12-15 at 6, on the translation's element: the matrix
		 * moved along its own axes, as many times as it is sent (a
		 * road's segments or the letters of a word, the same model
		 * called again after each) */
		if ((inst[0] & 0x000FF000) == 0x00036000) {
			const float x = *(float *) &inst[1];
			const float y = *(float *) &inst[2];
			const float z = *(float *) &inst[3];
			unsigned j;

			for (j = 0; j < 3; j++)
				mv->mtx[3][j] += x * mv->mtx[0][j] + y * mv->mtx[1][j] + z * mv->mtx[2][j];
			break;
		}
		/* Ignore conditional version. PHARRIER's and SWRACER's attracts
		 * send 30 to 1300 a frame */
		if (inst[0] & 0x0008F000) {
			HL_ERROR_FEW ("@%08X: conditional modelview", PC);
			return;
		}

		push = (inst[0] >> 18) & 1;
		elem = (inst[0] >> 16) & 3;
		if (push)
			mv = &gpu->link.scratch;

		/* First element during upload. */
		if (elem == 3) {
			unsigned i, j;

			for (i = 0; i < 4; i++)
				for (j = 0; j < 4; j++)
					mv->mtx[i][j] = (i == j) ? 1.0f : 0.0;
		}

		/* We store the columns as rows to facilitate the
		 * translation to GL column-major convertion in the
		 * renderer. */
		mv->mtx[elem][0] = *(float *) &inst[1];
		mv->mtx[elem][1] = *(float *) &inst[2];
		mv->mtx[elem][2] = *(float *) &inst[3];
		mv->mtx[elem][3] = (elem == 3) ? 1.0f : 0.0f;

		/* Last element during upload. */
		if (elem == 0) {
			if (push) {
				gpu->link.pushed = *mv;
				gpu->link.has_pushed = true;
			} else {
				MV.total = MV.depth + 1;
				MV.depth = 0;
				set_link (gpu);
			}
			HL_LOG ("DEBUG MTX depth=%u total=%u", MV.depth, MV.total);
		}
		break;
	case 0x5:
		{
			vec4f_t v, w;

			v[0] = *(float *) &inst[1];
			v[1] = *(float *) &inst[2];
			v[2] = *(float *) &inst[3];
			v[3] = 1.0f;

			mult_mtx4x4f_vec4f (w, MV.table[MV.depth].mtx, v);
			/* DRAW_DISTANCE: the models that much nearer for
			 * their levels of detail */
			LOD.value = norm_vec4 (w) / hl::Config::get ()->draw_distance;
			/* The point's distance from the eye */
			LOD.distance = sqrtf (w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) /
			               hl::Config::get ()->draw_distance;
		}
		break;
	case 0x9:
	case 0xB:
		switch (inst[0] & 0x000FF000) {
		case 0x00008000: /* Direction */
			lit->direction[0] = *(float *) &inst[1];
			lit->direction[1] = *(float *) &inst[2];
			lit->direction[2] = *(float *) &inst[3];
			lit->has_direction = 1;
			break;
		case 0x00010000: /* Position */
			lit->position[0] = *(float *) &inst[1];
			lit->position[1] = *(float *) &inst[2];
			lit->position[2] = *(float *) &inst[3];
			lit->has_position = 1;
			break;
		case 0x00016000: /* Use old position */
			/* XXX TODO */
			lit->position[0] = 0.0f;
			lit->position[1] = 0.0f;
			lit->position[2] = 0.0f;
			lit->has_position = 1;
			break;
		default:
			HL_ERROR ("CP @%08X: unhandled light 161 param: %08X",
			          PC, inst[0]);
			break;
		}
		break;
	default:
		HL_ASSERT (0);
		break;
	}
}

D (0x161)
{
	uint32_t push, elem;

	switch ((inst[0] >> 8) & 0xF) {

	case 1:
		push = (inst[0] >> 18) & 1;
		elem = (inst[0] >> 16) & 3;

		UNHANDLED |= !!(inst[0] & 0xFFF0F000);

		DISASM ("mtx: set vector [%c %u (%f %f %f)]",
		        push ? 'P' : ' ', elem,
		        *(float *) &inst[1], *(float *) &inst[2], *(float *) &inst[3]);
		break;
	case 5:
		UNHANDLED |= !!(inst[0] & 0xFFFC0000);

		DISASM ("lod: set vector [%f %f %f]",
		        *(float *) &inst[1], *(float *) &inst[2], *(float *) &inst[3]);
		break;
	case 9:
		UNHANDLED |= !!(inst[0] & 0xFFFE0000);

		DISASM ("lit: set vector 9 [%f %f %f]",
		        *(float *) &inst[1], *(float *) &inst[2], *(float *) &inst[3]);
		break;
	case 0xB:
		UNHANDLED |= !!(inst[0] & 0xFFFF0000);

		DISASM ("lit: set vector B [%f %f %f]",
		        *(float *) &inst[1], *(float *) &inst[2], *(float *) &inst[3]);
		break;
	default:
		HL_ASSERT (0);
		break;
	}
}

/*
 * Materials
 * =========
 *
 * It supports flat, diffuse and phong shading. XXX more to come.
 */

static uint32_t
get_material_index (uint32_t *inst)
{
	return (inst[0] >> 16) & (NUM_MATERIALS - 1);
}

/* 091	Material: Set Diffuse
 *
 *	-------- -------- -------o oooooooo
 *	-------- BBBBBBBB GGGGGGGG RRRRRRRR
 *
 * See PH:@0C0CF742.
 *
 *
 * 291	Material: Set Ambient
 *
 *	-------- -------- -------o oooooooo
 *	-------- BBBBBBBB GGGGGGGG RRRRRRRR
 *
 * See PH:@0C0CF742.
 *
 *
 * 491	Material: Set Specular/Shininess
 *
 *	-------- -------- -------o oooooooo
 *	SSSSSSSS BBBBBBBB GGGGGGGG RRRRRRRR
 *
 * S = Shininess
 *
 * The colour has 6 bits: 3F at most.
 *
 * See PH:@0C0CF798, PH:@0C01782C.
 *
 *
 * 691	Material: Set Ambient Reflectance
 *
 *	RRRRRRRR RRRRRRRR -------o oooooooo
 *	BBBBBBBB BBBBBBBB GGGGGGGG GGGGGGGG
 *
 * What the scene's ambient light is multiplied by, FF being 1 (7F is
 * common). The "ambient" of 291 is not this: it is a colour of the
 * texture modes (see 2C1).
 *
 * See PH:@0C0CF7CC.
 *
 * NOTE: A91 and C91 are used in BRAVEFF title screen. They clearly alias A81
 * and C81.
 */

static void hikaru_gpu_inst_0x081 (Gpu *, uint32_t *);
static void hikaru_gpu_disasm_0x081 (Gpu *, uint32_t *);

I (0x091)
{
	hikaru_material_t *mat = &MAT.scratch;

	switch ((inst[0] >> 8) & 15) {
	case 0:
		mat->diffuse[0] = inst[1] & 0xFF;
		mat->diffuse[1] = (inst[1] >> 8) & 0xFF;
		mat->diffuse[2] = (inst[1] >> 16) & 0xFF;
		mat->has_091 = 1;
	case 2:
		mat->ambient[0] = inst[1] & 0xFF;
		mat->ambient[1] = (inst[1] >> 8) & 0xFF;
		mat->ambient[2] = (inst[1] >> 16) & 0xFF;
		mat->has_291 = 1;
		break;
	case 4:
		mat->specular[0] = inst[1] & 0xFF;
		mat->specular[1] = (inst[1] >> 8) & 0xFF;
		mat->specular[2] = (inst[1] >> 16) & 0xFF;
		mat->specular[3] = inst[1] >> 24;
		mat->has_491 = 1;
		break;
	case 6:
		mat->unknown[0] = inst[0] >> 16;
		mat->unknown[1] = inst[1] & 0xFFFF;
		mat->unknown[2] = inst[1] >> 16;
		mat->has_691 = 1;
		break;
	case 0xA:
	case 0xC:
		hikaru_gpu_inst_0x081 (gpu, inst);
		/* XXX HACK */
		PC -= 4;
		return;
	default:
		HL_ASSERT (0);
		break;
	}
}

D (0x091)
{
	switch ((inst[0] >> 8) & 15) {
	case 0:
		UNHANDLED |= !!(inst[0] & 0xFFFFF800);
	
		DISASM ("mat: set diffuse [%08X]", inst[1]);
		break;
	case 2:
		UNHANDLED |= !!(inst[0] & 0xFFFFF800);
	
		DISASM ("mat: set ambient [%08X]", inst[1]);
		break;
	case 4:
		UNHANDLED |= !!(inst[0] & 0xFFFFF800);
	
		DISASM ("mat: set specular [%X]", inst[1]);
		break;

	case 6:
		UNHANDLED |= !!(inst[0] & 0x0000F800);
	
		DISASM ("mat: set unknown [%X %X %X]",
		        inst[0] >> 16, inst[1] >> 16, inst[1] & 0xFFFF);
		break;
	case 0xA:
	case 0xC:
		hikaru_gpu_disasm_0x081 (gpu, inst);
		break;
	default:
		HL_ASSERT (0);
		break;
	}
}

/* 081	Material: Set Unknown
 *
 *	-------- ----mmmm ---n---o oooooooo
 *
 * n, m = Unknown
 *
 *
 * 881	Material: Set Flags
 *
 *	XXXXXXXX xyhatzSS -------o oooooooo
 *
 * X = Unknown
 *
 *	Used in BRAVEFF.
 *
 * x = Has X.
 *
 * y = Unknown.
 *
 *	Used in BRAVEFF.
 *
 * S = Shading mode
 *
 *	0 = Unlit.
 *	1 = Lit: the lights, plus the scene's ambient light times 291 and
 *	    691 (PHARRIER's asteroids, 291 olive: their shade olive and dark
 *	    on the board).
 *	2 = Lit: the lights, plus 691 alone (used for FX and some
 *	    geometry in PHARRIER: its flames orange, magenta with 291).
 *
 *	Lighting multiplies the base colour (see 2C1's mode) and adds the
 *	specular. With all its lights off, a lit material still takes its
 *	ambient.
 *
 * z = Depth blend (fog)
 *
 *	Probably decides whether the material is affected by fog, and that's it.
 *
 * t = Textured
 *
 * a = Alpha mode
 *
 *	The alpha mixes the diffuse with the texel (the texel's alpha
 *	then the alpha), or with the ambient (291) if untextured.
 *	Apparently only used for the skate in AIRTRIX.
 *
 * h = Highlight mode
 *
 *	Used for the player character in PHARRIER.
 *
 * See PH:@0C0CF700.
 *
 *
 * A81	Material: Set Blending Mode
 *
 *	-------- ------mm -------o oooooooo
 *
 * m = Blending Mode
 *
 *	How a mesh of poly type 5 or 6 (103) blends with the scene:
 *
 *	0 = By its alpha (almost always).
 *	1 = Times the scene.
 *	2 = Added, by its alpha: lights, star patches and trick trails in
 *	    AIRTRIX (the glow where a trail meets the skater).
 *	3 = Added, the scene dimmed by its alpha.
 *
 * See PH:@0C0CF7FA.
 *
 *
 * C81	Material: Set Alpha Test
 *
 *	-------- --IIIIII -------o oooooooo
 *
 * I = Index intop the alpha threshold table (see instruction 154).
 *
 * See PH:@0C0CF868-@0C0CF876.
 */

I (0x081)
{
	hikaru_material_t *mat = &MAT.scratch;

	switch ((inst[0] >> 8) & 0xF) {
	case 0x0:
		mat->_081 = inst[0];
		mat->has_081 = 1;
		break;
	case 0x8:
		mat->_881 = inst[0];
		mat->has_881 = 1;
		break;
	case 0xA:
		mat->_A81 = inst[0];
		mat->has_A81 = 1;
		break;
	case 0xC:
		mat->_C81 = inst[0];
		mat->has_C81 = 1;
		break;
	}
}

D (0x081)
{
	switch ((inst[0] >> 8) & 0xF) {
	case 0:
		UNHANDLED |= !!(inst[0] & 0xFFF0E000);

		DISASM ("mat: set unknown");
		break;
	case 8:
		UNHANDLED |= !!(inst[0] & 0x0000F000);

		DISASM ("mat: set flags [mode=%u zblend=%u tex=%u alpha=%u highl=%u y=%u x=%u X=%02X]",
		        (inst[0] >> 16) & 3,
		        (inst[0] >> 18) & 1,
		        (inst[0] >> 19) & 1,
		        (inst[0] >> 20) & 1,
		        (inst[0] >> 21) & 1,
		        (inst[0] >> 22) & 1,
		        (inst[0] >> 23) & 1,
		        inst[0] >> 24);
		break;
	case 0xA:
		UNHANDLED |= !!(inst[0] & 0xFFFCF000);

		DISASM ("mat: set blending mode [mode=%u]", (inst[0] >> 16) & 3);
		break;
	case 0xC:
		UNHANDLED |= !!(inst[0] & 0xFFC0F000);

		DISASM ("mat: set unknown [%x %X]",
		        (inst[0] >> 21) & 1, (inst[0] >> 16) & 0x1F);
		break;
	default:
		HL_ASSERT (0);
		break;
	}
}

/* 084	Commit Material
 *
 *	------nn nnnnnnnn ---1---o oooooooo
 *
 * n = Index
 *
 * See PH:@0C0153D4, PH:@0C0CF878.
 */

I (0x084)
{
	uint32_t index = MAT.base + get_material_index (inst);

	if (index >= NUM_MATERIALS) {
		HL_ERROR ("CP: material commit index exceeds MAX (%u >= %u), skipping",
		          index, NUM_MATERIALS);
		return;
	}

	MAT.table[index] = MAT.scratch;
}

D (0x084)
{
	UNHANDLED |= !!(inst[0] & 0xFF00E000);
	UNHANDLED |= !(inst[0] & 0x1000);

	DISASM ("mat: commit @base+%u", get_material_index (inst));
}

/* 083	Recall Material
 *
 *	-nnnnnnn nnnnnnnn ---A---o oooooooo
 *
 * n = Index, 15 bits: VON4 sets its robot's base at 5005, its HUD's at
 *     1004 (the robot took the HUD's flat black and cyan while bit 14 was
 *     dropped); its bases go up to 7832
 * A = Active
 *
 * See @0C00657C, PH:@0C0CF882.
 */

I (0x083)
{
	uint32_t index = get_material_index (inst);

	if (!(inst[0] & 0x1000))
		MAT.base = index;
	else {
		index += MAT.base;
		if (index >= NUM_MATERIALS) {
			HL_ERROR ("CP: material recall index exceeds MAX (%u >= %u), skipping",
			          index, NUM_MATERIALS);
			return;
		}

		MAT.scratch = MAT.table[index];
		if (gpu->state.in_mesh)
			((Renderer *) gpu->renderer)->split_mesh (PC);
	}
}

D (0x083)
{
	if (!(inst[0] & 0x1000)) {
		UNHANDLED |= !!(inst[0] & 0x8000E000);

		DISASM ("mat: set base %u", get_material_index (inst));
	} else {
		UNHANDLED |= !!(inst[0] & 0xFF00E000);

		DISASM ("mat: recall @base+%u", get_material_index (inst));
	}
}

/*
 * Texheads
 * ========
 *
 * Textures used for 3D rendering are stored (through the GPU IDMA) in the two
 * available TEXRAM banks.
 */

static uint32_t
get_texhead_index (uint32_t *inst)
{
	return (inst[0] >> 16) & (NUM_TEXHEADS - 1);
}

/* 0C1	Texhead: Set Bias
 *
 *	----BBBB AAAA--MM -------o oooooooo
 *
 * A, B = Unknown values.
 * M = Unknown mode.
 *
 *	Mode 0 is used frequently with values 0 and FF.
 *
 *	Mode 1 is used with a variety of values.
 *
 *	Mode 2 is only used in BRAVEFF.
 *
 *
 * 2C1	Texhead: Set Format/Size
 *
 *	MMMFFFrr wwHHHWWW ba-----o oooooooo
 *
 * M = Mode: how the texel combines with the material's colours.
 *
 *	0 = The texel.
 *	1 = The texel over the diffuse (091), by the texel's alpha.
 *	2 = The texel times the ambient (291). Most of AIRTRIX's
 *	    scenery: its "restaurant" sign is a white texture with a
 *	    light blue ambient.
 *	4 = The diffuse, with the texel's alpha.
 *	5 = From the diffuse to the ambient, by the texel.
 *
 *	The material's lighting then applies to the result. Modes 1, 4 and 5
 *	are rare; 3 is unknown.
 *
 * a = Texcoords unit: whole texels if set, 1/16 texel (12.4) otherwise.
 *     In SGNASCAR it is 1 for hi-res LODs, 0 for lo-res LODs; its driver
 *     numbers (32 for a 32x32 texture) and checkers have it too.
 *
 * b = Unknown: all of PHARRIER's texheads, none of BRAVEFF's; SGNASCAR's
 *     smoke has it. Once taken for what lets 0D1's panning move the
 *     texture, which the vertices pick instead (see 0D1).
 *
 * F = Format
 *
 * r = Repeat mode
 *
 *	0 = Normal repeat.
 *	1 = Mirrored repeat.
 *
 *	Bit 0 for V, bit 1 for U. Only meaningful if wrapping is enabled.
 *
 * w = Wrap mode
 *
 *	0 = Clamp.
 *	1 = Wrap.
 *
 *	Bit 0 for V, bit 1 for U.
 *
 * H = log16 of Height 
 * W = log16 of Width
 *
 * See PH:@0C015BCC.
 *
 *
 * 4C1	Texhead: Set Slot
 *
 *	nnnnnnnn mmmmmmmm ---b---o oooooooo
 *
 * n = Slot Y
 * m = Slot X
 * b = TEXRAM bank
 *
 * NOTE: for some reason the BOOTROM uploads a couple of 2C1/4C1 instructions
 * with their parameters swapped. This hasn't been seen in any game so far.
 *
 * See PH:@0C015BA0.
 */

I (0x0C1)
{
	hikaru_texhead_t *th = &TEX.scratch;

	switch ((inst[0] >> 8) & 7) {
	case 0:
		th->_0C1 = inst[0];
		th->has_0C1 = 1;
		break;
	case 2:
		th->_2C1 = inst[0];
		th->has_2C1 = 1;
		break;
	case 4:
		th->_4C1 = inst[0];
		th->has_4C1 = 1;
		break;
	default:
		HL_ASSERT (0);
		break;
	}
}

D (0x0C1)
{
	switch ((inst[0] >> 8) & 7) {
	case 0:
		UNHANDLED |= !!(inst[0] & 0xF00CF800);

		DISASM ("tex: set bias [mode=%u %X]",
		        (inst[0] >> 16) & 0xF, (inst[0] >> 20) & 0xFF);
		break;

	case 2:
		UNHANDLED |= !!(inst[0] & 0x00003800);

		DISASM ("tex: set format [%ux%u fmt=%u wrap=(%u %u|%u %u) mode=%u unk=%X]",
		        16 << ((inst[0] >> 16) & 7),
		        16 << ((inst[0] >> 19) & 7),
		        (inst[0] >> 26) & 7,
		        (inst[0] >> 22) & 1,
		        (inst[0] >> 23) & 1,
		        (inst[0] >> 24) & 1,
		        (inst[0] >> 25) & 1,
		        inst[0] >> 29,
		        (inst[0] >> 14) & 3);
		break;
	case 4:
		DISASM ("tex: set slot [bank=%u (%X,%X)]",
		        (inst[0] >> 12) & 1,(inst[0] >> 16) & 0xFF, inst[0] >> 24);

		UNHANDLED |= !!(inst[0] & 0x0000E000);
		break;
	default:
		HL_ASSERT (0);
		break;
	}
}

/* 0D1	Texhead: Set Panning
 *
 *	-------- ------MM -------o oooooooo
 *	YYYYYYYY YYYYYYYY XXXXXXXX XXXXXXXX
 *
 * M = Offset slot, 0 to 3
 * X, Y = Signed amounts, in 1/16 texel
 *
 * A vertex adds the slot its header picks (bits 19:18) to its texcoords;
 * slot 0 is kept but never added. As Odin has it, by Réda Chérif-Touil,
 * from a port of the games. The slots stay until set again: SGNASCAR
 * scrolls a texture of its menus with slot 1 and sets none in a race,
 * where its smoke's vertices pick no slot (a texture moved by whatever was
 * left, the smoke was a white square).
 */

I (0x0D1)
{
	unsigned m = (inst[0] >> 16) & 3;

	gpu->texoffset[m][0] = (int16_t) inst[1];
	gpu->texoffset[m][1] = (int16_t) (inst[1] >> 16);
}

D (0x0D1)
{
	UNHANDLED |= !!(inst[0] & 0xFFFCF000);

	DISASM ("tex: set panning [%u %+d %+d]",
	        (inst[0] >> 16) & 3, (int16_t) inst[1], (int16_t) (inst[1] >> 16));
}

/* 0C4	Commit Texhead
 *
 *	------nn nnnnnnnn ---uoooo oooooooo
 *
 * n = Index
 * u = Unknown; always 1?
 *
 * See PH:@0C01545C. 
 */

I (0x0C4)
{
	uint32_t index = TEX.base + get_texhead_index (inst);

	if (index >= NUM_TEXHEADS) {
		HL_ERROR ("CP: texhead commit index exceedes MAX (%u >= %u), skipping",
		          index, NUM_TEXHEADS);
		return;
	}

	TEX.table[index] = TEX.scratch;
}

D (0x0C4)
{
	UNHANDLED |= !!(inst[0] & 0xFF00E000);
	UNHANDLED |= ((inst[0] & 0x1000) != 0x1000);

	DISASM ("tex: commit @base+%u", get_texhead_index (inst));

}

/* 0C3	Recall Texhead
 *
 *	-nnnnnnn nnnnnnnn ---M---o oooooooo
 *
 * n = Index, 15 bits as 083's (VON4's bases up to 7838)
 * M = Modifier: 0 = set base only, 1 = recall for real
 */

I (0x0C3)
{
	uint32_t index = get_texhead_index (inst);

	if (!(inst[0] & 0x1000))
		TEX.base = index;
	else {
		index += TEX.base;
		if (index >= NUM_TEXHEADS) {
			HL_ERROR ("CP: texhead recall index exceeds MAX (%u >= %u), skipping",
			          index, NUM_TEXHEADS);
			return;
		}

		TEX.scratch = TEX.table[index];
		if (gpu->state.in_mesh)
			((Renderer *) gpu->renderer)->split_mesh (PC);
	}
}

D (0x0C3)
{
	if (!(inst[0] & 0x1000)) {
		UNHANDLED |= !!(inst[0] & 0x8000E000);

		DISASM ("tex: set base %u", get_texhead_index (inst));
	} else {
		UNHANDLED |= !!(inst[0] & 0xFF00E000);

		DISASM ("tex: recall @base+%u", get_texhead_index (inst));
	}
}

/*
 * Lights
 * ======
 *
 * According to the system16.com specs, the hardware supports 1024 lights per
 * scene, and 4 lights per polygon. It supports several light types (ambient,
 * spot, etc.) and several emission types (constant, infinite linear, square,
 * reciprocal, and reciprocal squared.)
 *
 * AFAICS, the hardware supports two light-related objects: lights and
 * lightsets. A light specifies the position, direction, emission properties
 * and more of a single light. A lightset specifies a set of (up to) four
 * lights that insist on the mesh being rendered. This setup is consistent with
 * the system16 specs.
 */

static uint32_t
get_light_index (uint32_t *inst)
{
	return (inst[0] >> 16) & (NUM_LIGHTS - 1);
}

static uint32_t
get_lightset_index (uint32_t *inst)
{
	return (inst[0] >> 16) & (NUM_LIGHTSETS - 1);
}

/* 061	Light: Set Attenuation
 *
 *	-------- ------TT -------o oooooooo
 *	PPPPPPPP PPPPPPPP PPPPPPPP PPPPPPPP
 *	QQQQQQQQ QQQQQQQQ QQQQQQQQ QQQQQQQQ
 *	-------- -------- -------- --------
 *
 * T = Attenuation type
 *
 *	0 = Linear (or infinite if P = Q = 1)
 *	1 = Square
 *	2 = Inverse linear
 *	3 = Inverse square
 *
 * P, Q = Attenuation parameters
 *
 * For attenuation type 0, P and Q are:
 *
 *  P = 1.0f / (FR4 - FR5)
 *  Q = -FR5
 *
 * For attenuation type 1, P and Q are:
 *
 *  P = 1.0f / (FR4**2 - FR5**2)
 *  Q = -FR5**2
 *
 * For attenuation type 2, P and Q are:
 *
 *  P = (FR4 * FR5) / (FR4 - FR5)
 *  Q = -1.0f / |FR5|
 *
 * For attenuation type 3, P and Q are:
 *
 *  P = (FR4**2 * FR5**2) / (FR5**2 - FR4**2)
 *  Q = -1.0 / |FR5**2|
 */

I (0x061)
{
	hikaru_light_t *lit = &LIT.scratch;

	lit->att_type       = (inst[0] >> 16) & 3;
	lit->attenuation[0] = *(float *) &inst[1];
	lit->attenuation[1] = *(float *) &inst[2];

	lit->has_061 = 1;
	lit->has_position = 0;
	lit->has_direction = 0;
}

D (0x061)
{
	UNHANDLED |= !!(inst[0] & 0xFFFCF000);

	DISASM ("lit: set attenuation [%u p=%f q=%f]",
	        (inst[0] >> 16) & 3, *(float *) &inst[1], *(float *) &inst[2]);
}

/* 051	Light: Set Diffuse
 *
 *	-------- D---AAAA -------o oooooooo
 *	--BBBBBB BBBBGGGG GGGGGGRR RRRRRRRR
 *
 * D = Unknown. BRAVEFF sets it on the light of its aiming reticles, which
 *     its cabinet shows lit: it does not switch the light off.
 * A = Ambient level, 0 to 15: the light adds A / 16 of its colour to every
 *     polygon it reaches, facing it or not, cut by its distance and its
 *     cone as the rest of its light. PHARRIER's light editor shows the
 *     field as "amb" next to the colour (PH:@0C1D7048); SGNASCAR sends
 *     16 times a level, 15 at most (@0C062200); VON4 leaves 0. As Odin's
 *     author, Réda Chérif-Touil, found it.
 *
 * See PH:@0C0178C6; for a,b,c computation see PH:@0C03DC66.
 *
 *
 * 451	Light: Set Specular
 *
 *	-------D -------- -------o oooooooo
 *	-------- BBBBBBBB GGGGGGGG RRRRRRRR
 *
 * D = Disabled.
 *
 * See PH:@0C017A7C, PH:@0C017B6C, PH:@0C017C58, PH:@0C017CD4, PH:@0C017D64.
 */

I (0x051)
{
	hikaru_light_t *lit = &LIT.scratch;

	switch ((inst[0] >> 8) & 7) {
	case 0:
		lit->_051_bit   = (inst[0] >> 23) & 1;
		lit->ambient_level = (inst[0] >> 16) & 0xF;
		lit->diffuse[0] = inst[1] & 0x3FF;
		lit->diffuse[1] = (inst[1] >> 10) & 0x3FF;
		lit->diffuse[2] = (inst[1] >> 20) & 0x3FF;
		lit->has_051 = 1;
		break;
	case 4:
		lit->has_specular  = ((inst[0] >> 24) & 1) ^ 1;
		lit->specular[0] = inst[1] & 0xFF;
		lit->specular[1] = (inst[1] >> 8) & 0xFF;
		lit->specular[2] = (inst[1] >> 16) & 0xFF;
		lit->has_451 = 1;
		break;
	default:
		HL_ASSERT (0);
		break;
	}
}

D (0x051)
{
	switch ((inst[0] >> 8) & 7) {
	case 0:
		UNHANDLED |= !!(inst[0] & 0xFF70F000);
		UNHANDLED |= !!(inst[1] & 0xC0000000);

		DISASM ("lit: set diffuse [%X]", inst[1]);
		break;
	case 4:
		UNHANDLED |= !!(inst[0] & 0xFEFFF000);
		
		DISASM ("lit: set specular [%X]", inst[1]);
		break;
	default:
		HL_ASSERT (0);
		break;
	}
}

/* 006	Light: Unknown
 *
 *	-------- -------- -------o oooooooo
 */

I (0x006)
{
}

D (0x006)
{
	UNHANDLED |= !!(inst[0] & 0xFFFFF000);

	DISASM ("lit: unknown");
}

/* 046	Light: Unknown
 *
 *	-------- -------n ----oooo oooooooo
 *
 * n = Unknown
 */

I (0x046)
{
}

D (0x046)
{
	UNHANDLED |= !!(inst[0] & 0xFFFEF000);

	DISASM ("lit: unknown [%u]", (inst[0] >> 16) & 1);
}

/* 104	Commit Light
 *
 *	------nn nnnnnnnn ----oooo oooooooo
 *
 * n = Index
 */

I (0x104)
{
	uint32_t index = get_light_index (inst);

	LIT.table[index] = LIT.scratch;
}

D (0x104)
{
	UNHANDLED |= !!(inst[0] & 0xFC00F000);

	DISASM ("lit: commit @%u", get_light_index (inst));
}

/* 064  Commit Lightset
 *
 *      -------- nnnnnnnn ---M---o oooooooo
 *      ------bb bbbbbbbb ------aa aaaaaaaa
 *      ------dd dddddddd ------cc cccccccc
 *	-------- -------- -------- -------- 
 *
 * M = Unknown (0 in the BOOTROM, 1 elsewhere)
 * n = Lightset index
 * a,b,c,d = Indices of four lights
 *
 * See PH:@0C017DF0.
 */

I (0x064)
{
	hikaru_lightset_t *ls;
	uint32_t index = LIT.base + get_lightset_index (inst);

	if (index >= NUM_LIGHTSETS) {
		HL_ERROR ("CP: lightset commit index exceeds MAX (%u >= %u), skipping",
		          index, NUM_LIGHTSETS);
		return;
	}

	ls = &LIT.sets[index];

	ls->lights[0] = LIT.table[inst[1] & (NUM_LIGHTS - 1)];
	ls->lights[1] = LIT.table[(inst[1] >> 16) & (NUM_LIGHTS - 1)];
	ls->lights[2] = LIT.table[inst[2] & (NUM_LIGHTS - 1)];
	ls->lights[3] = LIT.table[(inst[2] >> 16) & (NUM_LIGHTS - 1)];
	ls->set = 1;
}

D (0x064)
{
	UNHANDLED |= !!(inst[0] & 0xFF00E000);
	UNHANDLED |= ((inst[0] & 0x1000) != 0x1000);
	UNHANDLED |= !!(inst[1] & 0xFC00FC00);
	UNHANDLED |= !!(inst[2] & 0xFC00FC00);

	DISASM ("lit: commit set @base+%u [%u %u %u %u]",
	        get_lightset_index (inst),
	        inst[1] & (NUM_LIGHTS - 1), (inst[1] >> 16) & (NUM_LIGHTS - 1),
	        inst[2] & (NUM_LIGHTS - 1), (inst[2] >> 16) & (NUM_LIGHTS - 1));
}

/* 043	Recall Lightset
 *
 *	----DDDD nnnnnnnn ---A---o oooooooo
 *
 * A = Active
 * D = Disabled lights mask
 * n = Index
 */

I (0x043)
{
	uint32_t index = get_lightset_index (inst);

	if (!(inst[0] & 0x1000))
		LIT.base = index;

	index += LIT.base;
	if (index >= NUM_LIGHTSETS) {
		HL_ERROR ("CP: lightset recall index exceeds MAX (%u >= %u), skipping",
		          index, NUM_LIGHTSETS);
		return;
	}

	LIT.scratchset = LIT.sets[index];
	LIT.scratchset.mask = (inst[0] >> 24) & 0xF;
}

D (0x043)
{
	UNHANDLED |= !!(inst[0] & 0xF000E000);

	if (!(inst[0] & 0x1000))
		DISASM ("lit: set set base %u", get_lightset_index (inst));
	else
		DISASM ("lit: recall set @base+%u [mask=%X]",
		        get_lightset_index (inst), (inst[0] >> 24) & 0xFF);
}

/*
 * Meshes
 * ======
 *
 * This class of instructions pushes (or otherwise deals with) vertex
 * data to the transformation/rasterization pipeline.
 */

/* 101	Mesh: Set Depth Bias
 *
 *	-----MBB BBBBBBBB -------o oooooooo
 *
 * B = Bias, toward the eye
 * M = Mode: 0 a constant, 1 the polygon's depth slope (see upload_depth)
 *
 * See @0C008040, PH:@0C016418, PH:@0C016446.
 *
 *
 * 301	Mesh: Set Unknown
 *
 *	-------- UNNNNNNN -------o oooooooo
 *
 * U, N = Unknown (N != 1)
 *
 *
 * 501	Mesh: Set Pattern
 *
 *	-------- ---PPPPP -------o oooooooo
 *
 * P = Dot pattern: the meshes that follow are drawn on some pixels of each
 *     4 x 4 square of the screen only, whatever their type. 0 is every
 *     pixel, 8 none: 1 to 7 leave out more and more, 8 + n draws what n
 *     leaves out; bit 4 mirrors the pattern left to right (the 32
 *     patterns: PATTERNS in hikaru-renderer.cpp). The objects that fade
 *     in or out show what lies behind them through the dots.
 *
 * AIRTRIX keeps the codes of nine levels (AT:@0C0C841C, set by
 * AT:@0C049BA6 in AT:@0C61404C) and wraps a model between a 501 and a 501 0
 * (AT:@0C049D08): its combo labels come and go in dots. PHARRIER draws the
 * player's shadow with C and 4 in turn, BRAVEFF its smoke; SWRACER sends
 * -7 to -1 on 16 bits for the far scenery it brings in (the low 5 bits
 * taken). A pattern holds across the material definitions, and a return
 * (082) takes back the caller's (the call, 052, keeps it): a model of
 * BRAVEFF's burning hall sets C in its own list and never clears it, while
 * the firemen that follow are drawn whole. Decoded by Odin (Réda
 * Chérif-Touil, GPL v3+), from the patterns of the Xbox 360 port.
 *
 *
 * 901	Mesh: Set Precision (Static)
 *
 *	-------- pppppppp ----100o oooooooo
 *
 * Information kindly provided by CaH4e3.
 */

I (0x101)
{
	uint32_t log;

	switch ((inst[0] >> 8) & 0xF) {
	case 1:
		gpu->depth_bias = inst[0] >> 16;
		break;
	case 3:
		break;
	case 5:
		gpu->pattern = (inst[0] >> 16) & 0x1F;
		break;
	case 9:
		log = (inst[0] >> 16) & 0xFF;
		/* 2^(log - 8D): above 8D the units grow (SWRACER), which a
		 * shift by a negative amount did not give */
		POLY.static_mesh_precision = ldexpf (1.0f, (int) log - 0x8D);
		break;
	default:
		HL_ASSERT (0);
		break;
	}
}

D (0x101)
{
	uint32_t log;
	float precision;

	switch ((inst[0] >> 8) & 0xF) {
	case 1:
		UNHANDLED |= !!(inst[0] & 0xF800F000);

		DISASM ("mesh: set depth bias [%u]", inst[0] >> 16);
		break;
	case 3:
		UNHANDLED |= !!(inst[0] & 0xFF00F000);

		DISASM ("mesh: set unknown [%u]", (inst[0] >> 16) & 0xFF);
		break;
	case 5:	
		UNHANDLED |= !!(inst[0] & 0xFFE0F000);

		DISASM ("mesh: set pattern [%X]", (inst[0] >> 16) & 0x1F);
		break;
	case 9:
		log = (inst[0] >> 16) & 0xFF;
		precision = ldexpf (1.0f, (int) log - 0x8D);

		UNHANDLED |= !!(inst[0] & 0xFF00F000);

		DISASM ("mesh: set precision s [%u %f]", log, precision);	
		break;
	default:
		HL_ASSERT (0);
		break;
	}

}

/* 103	Set Poly Type
 * 113	Set Poly Type
 *
 *	AAAAAAAA -------- -------o oooxoooo
 *
 * A = Base mesh alpha value
 * x = Unknown
 *
 *  3: Opaque. Alpha is ignored.
 *  9: Punch-through. Alpha is ignored.
 *  D: Translucent.
 *
 * The type is bits 9-11 (1 opaque, 4 punch-through, 6 translucent). Type 4
 * is alpha-tested (154, C81) and opaque, drawn before the translucent ones.
 * Types 5 and 6 blend with the scene by their material's blending mode
 * (A81).
 *
 * Information kindly contributed by DreamZzz.
 *
 * See AT:@0C049CDA, PH:@0C0173CA, AT:@0C69A220.
 */

static void
get_poly_type (uint32_t *inst, uint32_t *type, float *alpha)
{
	*type	= (inst[0] >> 9) & 7;
	*alpha	= (inst[0] >> 24) * (1.0f / 255.0f);
}

I (0x103)
{
	uint32_t type;
	float alpha;

	get_poly_type (inst, &type, &alpha);

	POLY.type  = type;
	POLY.alpha = alpha;
}

D (0x103)
{
	static const char *poly_type_name[8] = {
		"invalid 0",
		"opaque",
		"shadow A",
		"shadow B",
		"transparent",
		"background",
		"translucent",
		"invalid 7"
	};
	uint32_t type;
	float alpha;

	get_poly_type (inst, &type, &alpha);

	UNHANDLED |= !!(inst[0] & 0x00FFF000);

	DISASM ("mesh: set poly type [%s alpha=%f]", poly_type_name[type], alpha);
}

/* 12x	Mesh: Push Position (Static)
 * 1Ax	Mesh: Push Position (Dynamic)
 * 1Bx	Mesh: Push All (Position, Normal, Texcoords) (Dynamic)
 *
 *
 * They appear to have a common 32-bit header:
 *
 *	AAAAAAAA C---2x-- uuuSTTTo oooootpW
 *
 * A = Vertex alpha
 *
 * C = Don't Cull
 *
 *     Disables face culling for this mesh.
 *
 * 2 = 2-sided
 *
 *     Used in PHARRIER (the big SEGA text).
 *
 * x = Unknown
 *
 *     No idea (use in PHARRIER for the player' shoulder lights).
 *
 * u = Unknown
 *
 *     No idea.
 *
 * S = Linked
 *
 *     The vertex takes the matrix pushed before the current one (161, P):
 *     the joints of BRAVEFF's characters and its flames, AIRTRIX's hands,
 *     shadows and trick trails (each piece from one of the board's last
 *     positions to the next).
 *
 * T = Triangle
 *
 *     The only observed values so far are 0 and 7.
 *
 *     If 0, the vertex is pushed to the GPU vertex buffer. If 7, the vertex is
 *     pushed and defines a triangle along with the two previously pushed
 *     vertices.
 *
 * t = Texcoord pivot?
 *
 *     Apparently only used by 1Bx, which includes texcoord info.
 *
 * p = Position pivot
 *
 *     When 0, the vertex is linked to the previous two according to the
 *     winding bit. When 1, the vertex at offset -2 is kept unchanged in the
 *     vertex buffer, and acts as a pivot for building a triangle fan.
 *
 * W = Winding
 *
 *     Triangle winding specifies whether the vertices composing the
 *     to-be-drawn triangle are to be in the order (0, -1, -2), and W is 0
 *     in this case, or (0, -2, -1), and W is 1 in this case.
 *
 *     If vertex in position -2 is a pivot, it is treated as if it wasn't. [?]
 *
 * For 12x, the rest of the instruction looks like:
 *
 *	-------- -------- -------- --------
 *	xxxxxxxx xxxxxxxx ??????uu uuuuuuuu
 *	yyyyyyyy yyyyyyyy ??????vv vvvvvvvv
 *	zzzzzzzz zzzzzzzz ??????ww wwwwwwww
 *
 * x,y,z = Position
 * u,v,w = Normal
 *
 * For 1BX, the rest of the instruction looks like:
 *
 *	-------- -------- -------- --------
 *	xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx
 *	yyyyyyyy yyyyyyyy yyyyyyyy yyyyyyyy
 *	zzzzzzzz zzzzzzzz zzzzzzzz zzzzzzzz
 *
 * x,y,z = Position
 *
 * For 1BX, the rest of the instruction looks like:
 *
 *	-------- -------- -------- --------
 *	xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx
 *	yyyyyyyy yyyyyyyy yyyyyyyy yyyyyyyy
 *	zzzzzzzz zzzzzzzz zzzzzzzz zzzzzzzz
 *	ssssssss ssssssss tttttttt tttttttt
 *	uuuuuuuu uuuuuuuu uuuuuuuu uuuuuuuu
 *	vvvvvvvv vvvvvvvv vvvvvvvv vvvvvvvv
 *	wwwwwwww wwwwwwww wwwwwwww wwwwwwww
 *
 * x,y,z = Position
 * u,v,w = Normal
 * s,t = Texcoords
 *
 * Meshes come in two flavors: dynamic and static. Dynamic meshes are specified
 * with normal IEEE457 floating-point values. Static meshes are specified with
 * variable fixed-point values; the fixed-point precision is defined by command
 * 901.
 *
 * Information on the existence of static/dynamic mesh varieties, and info on
 * the fixed-point decoding kindly provided by CaH4e3.
 */

I (0x12C)
{
	hikaru_vertex_t v;

	v.info.full = inst[0];

	HL_ASSERT (POLY.static_mesh_precision > 0.0f);

	v.body.position[0] = (int16_t)(inst[1] >> 16) * POLY.static_mesh_precision;
	v.body.position[1] = (int16_t)(inst[2] >> 16) * POLY.static_mesh_precision;
	v.body.position[2] = (int16_t)(inst[3] >> 16) * POLY.static_mesh_precision;

	v.body.normal[0] = (int16_t)((inst[1] & 0x3FF) << 6) / 16384.0f;
	v.body.normal[1] = (int16_t)((inst[2] & 0x3FF) << 6) / 16384.0f;
	v.body.normal[2] = (int16_t)((inst[3] & 0x3FF) << 6) / 16384.0f;

	if (gpu->skin.sp)
		skin_vertex (gpu, &v, true);
	((Renderer *) HR)->push_vertices (&v, HR_PUSH_POS | HR_PUSH_NRM, 1);
}

D (0x12C)
{
	UNHANDLED |= !!(inst[0] & 0x007F0000);

	DISASM ("mesh: push position s");
}

I (0x1AC)
{
	hikaru_vertex_t v;

	v.info.full = inst[0];

	v.body.position[0] = *(float *) &inst[1];
	v.body.position[1] = *(float *) &inst[2];
	v.body.position[2] = *(float *) &inst[3];

	if (gpu->skin.sp)
		skin_vertex (gpu, &v, false);
	((Renderer *) HR)->push_vertices (&v, HR_PUSH_POS, 1);
}

D (0x1AC)
{
	UNHANDLED |= !!(inst[0] & 0x007F0000);

	DISASM ("mesh: push position d");
}

I (0x1B8)
{
	hikaru_vertex_t v;

	v.info.full = inst[0];

	v.body.position[0] = *(float *) &inst[1];
	v.body.position[1] = *(float *) &inst[2];
	v.body.position[2] = *(float *) &inst[3];

	v.body.normal[0] = *(float *) &inst[5];
	v.body.normal[1] = *(float *) &inst[6];
	v.body.normal[2] = *(float *) &inst[7];

	v.body.texcoords[0] = ((int16_t) inst[4]) / 16.0f;
	v.body.texcoords[1] = ((int16_t) (inst[4] >> 16)) / 16.0f;

	if (gpu->skin.sp)
		skin_vertex (gpu, &v, true);
	((Renderer *) HR)->push_vertices (&v, HR_PUSH_POS | HR_PUSH_NRM | HR_PUSH_TXC, 1);
}

D (0x1B8)
{
	UNHANDLED |= !!(inst[0] & 0x00770000);

	DISASM ("mesh: push all d");
}

/* 0E8	Mesh: Push Texcoords 3
 *
 *	-------- -------n ----WWWo oooooooC
 *	vvvvvvvv vvvvvvvv uuuuuuuu uuuuuuuu
 *	vvvvvvvv vvvvvvvv uuuuuuuu uuuuuuuu
 *	vvvvvvvv vvvvvvvv uuuuuuuu uuuuuuuu
 *
 * The interaction of U, u, P, W, with the ones specified by the push position/
 * push all instructions is still unknown.
 *
 * n = The texcoords are the texhead's after the base's (base + 1), not the
 *     one recalled. hikcheck's C.R.T. TEST PAGE#2: its text with texhead 0
 *     (the font), then a quad over the picture with n set, its texcoords in
 *     whole texels of texhead 1 (16x16, wrapped): the crosshatch. No game's
 *     attract sets it. Which one it picks when the texhead recalled is not
 *     the base's is not known.
 *
 * u, v = Texcoords for three points.
 */

I (0x0E8)
{
	hikaru_vertex_t vs[3];
	hikaru_texhead_t recalled;
	bool next = (inst[0] >> 16) & 1;
	unsigned i;

	/* The texhead after the base's for these texcoords and their mesh */
	if (next) {
		recalled = TEX.scratch;
		TEX.scratch = TEX.table[(TEX.base + 1) & (NUM_TEXHEADS - 1)];
	}
	((Renderer *) HR)->pick_texhead (next, PC);

	for (i = 0; i < 3; i++) {
		vs[i].info.full = inst[0];

		vs[i].body.texcoords[0] = ((int16_t) inst[i+1]) / 16.0f;
		vs[i].body.texcoords[1] = ((int16_t) (inst[i+1] >> 16)) / 16.0f;
	}

	((Renderer *) HR)->push_vertices (&vs[0], HR_PUSH_TXC, 3);
	if (next)
		TEX.scratch = recalled;
}

D (0x0E8)
{

	UNHANDLED |= !!(inst[0] & 0xFFFEF000);

	DISASM ("mesh: push texcoords 3");
}

/* 158	Mesh: Push Texcoords 1
 *
 *	-------- ?------- ----???o ooooo??C
 *	vvvvvvvv vvvvvvvv uuuuuuuu uuuuuuuu
 *
 * u, v = Texcoords for one point.
 */

I (0x158)
{
	hikaru_vertex_t v;

	v.info.full = inst[0];

	v.body.texcoords[0] = ((int16_t) inst[1]) / 16.0f;
	v.body.texcoords[1] = ((int16_t) (inst[1] >> 16)) / 16.0f;

	((Renderer *) HR)->push_vertices (&v, HR_PUSH_TXC, 1);
}

D (0x158)
{
	UNHANDLED |= !!(inst[0] & 0xFF7FF000);

	DISASM ("mesh: push texcoords 1");
}

/****************************************************************************
 Unknown
****************************************************************************/

/* 181	FB: Set Blending
 *
 *	-------E AAAAAAAA -------o oooooooo
 *
 * E = Enable blending
 * A = Blending factor
 *
 *
 * 781	FB: Set Combiner
 *
 *	-----ENN -----enn -------o oooooooo
 *
 * E = Buffer A, Enable (?)
 * N = Buffer A, Select first buffer
 *
 * e = Buffer B, Enable (?)
 * n = Buffer B, Select second buffer
 *
 * Determines how to linearly combine the framebuffers to obtain the 3D scene.
 * The equation is given by:
 *
 *    result = factor * A + (1 - factor) * B
 *
 * Here A and B can be either the front/back buffer or a 2D layer. The numering
 * should be the same as that of registers 1A000180-1A00019C.
 *
 * The values uploaded here depend on the state of 1A00001C bits 23:24 and
 * 1A000020 bit 0. See @0C0065D6, PH:@0C016336, PH:@0C038952, PH:@0C015B50.
 */

I (0x181)
{
	switch ((inst[0] >> 8) & 7) {
	case 1:
		gpu->fb_config._181 = inst[0];
		break;
	case 7:
		gpu->fb_config._781 = inst[0];
		break;
	default:
		HL_ASSERT (0);
		break;
	}
}

D (0x181)
{
	switch ((inst[0] >> 8) & 7) {
	case 1:
		UNHANDLED |= !!(inst[0] & 0xFE00F800);

		DISASM ("fb: set blending (%u %X)",
		        (inst[0] >> 24) & 1, (inst[0] >> 16) & 0xFF);
		break;
	case 7:
		UNHANDLED |= !!(inst[0] & 0xF8F8F800);

		DISASM ("fb: set combiner (%X %X)",
		        (inst[0] >> 24) & 7, (inst[0] >> 16) & 7);
		break;
	default:
		HL_ASSERT (0);
		break;
	}
}

/* 088	Flush
 *
 *	-------- U------- ----xxxo oooooooo
 *
 * U = Unknown
 * x = Unknown
 *
 * Always comes as the last instruction. Perhaps some kind of 'flush all'
 * or 'raise IRQ' command or something like that. If it is a 'flush all'
 * command, it may set some GPU ports not set by 1C2 (1A000024 perhaps.)
 */

I (0x088)
{
}

D (0x088)
{
	UNHANDLED |= !!(inst[0] & 0xFF7FF000);

	DISASM ("unk: unknown");
}

/* 154	Mat: Set Alpha Threshold
 *
 *	-------- --IIIIII -------o oooooooo
 *	HHHHHHHH HHHHHHHH HHHHHHHH LLLLLLLL
 *
 * I = Index
 * L = Alpha low threshold
 * H = Alpha high threshold: its low byte (the others are FFFF or 0)
 *
 * A punch-through mesh (poly type 4) drops its pixels with an alpha below
 * L / 255 or above H / 255, and draws the others opaque. AIRTRIX's entry
 * 32 is 7F..FF; SGNASCAR's, SWRACER's and PHARRIER's 80..FF.
 *
 * See PH:@0C017798, PH:@0C0CF868. Used by instruction C81.
 */

I (0x154)
{
	ATABLE[(inst[0] >> 16) & 0x3F].full = inst[1];
}

D (0x154)
{
	UNHANDLED |= !!(inst[0] & 0xFFC0F000);

	DISASM ("mat: set alpha thresh [%u (%X %X)]",
	        (inst[0] >> 16) & 0x3F, inst[1] & 0xFF, inst[1] >> 8);
}

/* 194	Light: Set Table
 *
 *	------NN ---MMMMM -------o oooooooo
 *	LLLLLLLL LLLLLLLL HHHHHHHH HHHHHHHH
 *
 * N, M = Indices
 * L = Data, Lo
 * H = Data, Hi
 *
 * NOTE: definitely related to lighting; possibly spotlight angles.
 *
 * See PH:@0C017A3E.
 */

I (0x194)
{
	uint32_t index1 = (inst[0] >> 24) & 3;
	uint32_t index2 = (inst[0] >> 16) & 0x1F;

	LTABLE[index1][index2].full = inst[1];
}

D (0x194)
{
	UNHANDLED |= !!(inst[0] & 0xFCE0F000);

	DISASM ("light: set table [%u:%u lo=%X hi=%X]",
	        (inst[0] >> 24) & 3, (inst[0] >> 16) & 0x1F,
	        inst[1] >> 16, inst[1] & 0xFFFF);
}

/* 3A1	Set Lo Addresses
 *
 *	-------- -------- -----01o oooooooo
 *	llllllll llllllll llllllll llllllll
 *	LLLLLLLL LLLLLLLL LLLLLLLL LLLLLLLL
 *      -------- -------- -------- --------
 *
 *
 * 5A1	Set Hi Addresses
 *
 *	-------- -------- -----10o oooooooo
 *	uuuuuuuu uuuuuuuu uuuuuuuu uuuuuuuu
 *	UUUUUUUU UUUUUUUU UUUUUUUU UUUUUUUU
 *      -------- -------- -------- --------
 *
 * l,L,h,H = Addresses? Possibly watermarks?
 *
 * See PH:@0C016308 for both.
 */

I (0x1A1)
{
}

D (0x1A1)
{
	UNHANDLED |= !!(inst[0] & 0xFFFFF000);
	UNHANDLED |= (inst[3] != 0);

	DISASM ("unk: set address [%08X %08X]", inst[1], inst[2]);
}

/****************************************************************************
 Opcodes
****************************************************************************/

#define K(op_, base_op_, flags_) \
	{ op_, flags_, hikaru_gpu_inst_##base_op_, hikaru_gpu_disasm_##base_op_ }

static const struct {
	uint32_t op;
	uint16_t flags;
	void (* handler)(Gpu *gpu, uint32_t *inst);
	void (* disasm)(Gpu *gpu, uint32_t *inst);
} insns_desc[] = {
	/* 0x00 */
	K(0x000, 0x000, FLAG_CONTINUE),
	K(0x003, 0x003, 0),
	K(0x004, 0x004, 0),
	K(0x005, 0x005, 0),
	K(0x006, 0x006, 0),
	K(0x011, 0x011, 0),
	K(0x012, 0x012, FLAG_JUMP),
	K(0x021, 0x021, 0),
	/* 0x40 */
	K(0x043, 0x043, 0),
	K(0x046, 0x046, 0),
	K(0x051, 0x051, 0),
	K(0x052, 0x052, FLAG_JUMP),
	K(0x055, 0x055, 0),
	K(0x061, 0x061, 0),
	K(0x064, 0x064, 0),
	/* 0x80 */
	K(0x081, 0x081, FLAG_CONTINUE),
	K(0x082, 0x082, FLAG_JUMP),
	K(0x083, 0x083, FLAG_CONTINUE),
	K(0x084, 0x084, 0),
	K(0x088, 0x088, 0),
	K(0x091, 0x091, FLAG_CONTINUE),
	K(0x095, 0x095, 0),
	/* 0xC0 */
	K(0x0C1, 0x0C1, 0),
	K(0x0C3, 0x0C3, FLAG_CONTINUE),
	K(0x0C4, 0x0C4, 0),
	K(0x0D1, 0x0D1, 0),
	K(0x0E8, 0x0E8, FLAG_PUSH),
	K(0x0E9, 0x0E8, FLAG_PUSH),
	/* 0x100 */
	K(0x101, 0x101, 0),
	K(0x103, 0x103, 0),
	K(0x104, 0x104, 0),
	K(0x113, 0x103, 0),
	K(0x12C, 0x12C, FLAG_PUSH | FLAG_STATIC),
	K(0x12D, 0x12C, FLAG_PUSH | FLAG_STATIC),
	K(0x12E, 0x12C, FLAG_PUSH | FLAG_STATIC),
	K(0x12F, 0x12C, FLAG_PUSH | FLAG_STATIC),
	/* 0x140 */
	K(0x154, 0x154, 0),
	K(0x158, 0x158, FLAG_PUSH),
	K(0x159, 0x158, FLAG_PUSH),
	K(0x15A, 0x158, FLAG_PUSH),
	K(0x15B, 0x158, FLAG_PUSH),
	K(0x161, 0x161, 0),
	/* 0x180 */
	K(0x181, 0x181, 0),
	K(0x191, 0x191, 0),
	K(0x194, 0x194, 0),
	K(0x1A1, 0x1A1, 0),
	K(0x1AC, 0x1AC, FLAG_PUSH),
	K(0x1AD, 0x1AC, FLAG_PUSH),
	K(0x1AE, 0x1AC, FLAG_PUSH),
	K(0x1AF, 0x1AC, FLAG_PUSH),
	K(0x1B8, 0x1B8, FLAG_PUSH),
	K(0x1B9, 0x1B8, FLAG_PUSH),
	K(0x1BA, 0x1B8, FLAG_PUSH),
	K(0x1BB, 0x1B8, FLAG_PUSH),
	K(0x1BC, 0x1B8, FLAG_PUSH),
	K(0x1BD, 0x1B8, FLAG_PUSH),
	K(0x1BE, 0x1B8, FLAG_PUSH),
	K(0x1BF, 0x1B8, FLAG_PUSH),
	/* 0x1C0 */
	K(0x1C2, 0x1C2, FLAG_JUMP)
};

#undef K

void
hikaru_gpu_cp_init (Gpu *gpu)
{
	unsigned i;
	for (i = 0; i < 0x200; i++) {
		insns[i].handler = NULL;
		insns[i].flags = FLAG_INVALID;
		disasm[i] = NULL;
	}
	for (i = 0; i < NUMELEM (insns_desc); i++) {
		uint32_t op = insns_desc[i].op;
		insns[op].handler = insns_desc[i].handler;
		insns[op].flags   = insns_desc[i].flags;
		disasm[op]        = insns_desc[i].disasm;
	}
}

} /* namespace hikaru */
