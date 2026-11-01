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

#ifndef __HL_X64_EMITTER_H__
#define __HL_X64_EMITTER_H__

#include <vector>

#include "hl/core.h"

/* x86-64 machine code, written by the dynamic recompilers: the instructions
 * they need and no more, encoded by hand (REX, ModRM, SIB). */

namespace x64 {

enum Reg {
	RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
	R8, R9, R10, R11, R12, R13, R14, R15
};

/* The condition codes, as in jcc and setcc */
enum Cond {
	CC_O, CC_NO, CC_B, CC_AE, CC_E, CC_NE, CC_BE, CC_A,
	CC_S, CC_NS, CC_P, CC_NP, CC_L, CC_GE, CC_LE, CC_G
};

/* Memory the host may run: code written there, then called */
class CodeArena {
public:
	explicit CodeArena (size_t size);
	~CodeArena ();
	CodeArena (const CodeArena &) = delete;
	CodeArena &operator= (const CodeArena &) = delete;

	uint8_t		*base () const { return mem; }
	uint8_t		*top () const { return mem + used; }
	size_t		 left () const { return size - used; }
	/* Takes n bytes, aligned to align; NULL when full */
	uint8_t		*take (size_t n, size_t align = 16);
	/* Forgets everything written */
	void		 reset () { used = 0; }
	/* Gives back what follows end, in the last block taken (its bound
	 * was more than the code written) */
	void		 trim (const uint8_t *end)
	{
		if (end >= mem && end <= mem + used)
			used = (size_t) (end - mem);
	}

private:
	uint8_t		*mem;
	size_t		 size, used;
};

/* A place in the code a jump goes to, once bound */
struct Label {
	uint8_t			*target = NULL;
	/* The jumps' rel32 fields that wait for it */
	std::vector<uint8_t *>	 waiting;
};

class Emitter {
public:
	Emitter (uint8_t *buf, size_t size) : start (buf), p (buf), end (buf + size) {}

	uint8_t		*here () const { return p; }
	size_t		 length () const { return (size_t) (p - start); }
	/* Wrote past the buffer: the code is not usable */
	bool		 overflowed () const { return over; }

	void		 push (Reg r);
	void		 pop (Reg r);
	void		 ret () { byte (0xC3); }

	/* 64-bit: dst = src; dst = imm */
	void		 mov (Reg dst, Reg src);
	void		 mov_imm64 (Reg dst, uint64_t imm);
	/* 32-bit: dst = imm (zero extended) */
	void		 mov_imm32 (Reg dst, uint32_t imm);
	/* dst = src + disp, 32 bits */
	void		 lea32 (Reg dst, Reg src, int32_t disp);
	/* 32-bit loads and stores at [base + disp] */
	void		 load32 (Reg dst, Reg base, int32_t disp);
	void		 store32 (Reg base, int32_t disp, Reg src);
	void		 store32_imm (Reg base, int32_t disp, uint32_t imm);
	/* [base + disp] += imm, 32 bits */
	void		 add32_mem_imm (Reg base, int32_t disp, int32_t imm);
	/* Compares [base + disp] with a register or an immediate */
	void		 cmp32_mem (Reg base, int32_t disp, Reg r);
	void		 cmp8_mem_imm (Reg base, int32_t disp, uint8_t imm);
	/* 32-bit and 64-bit register arithmetic */
	void		 add64_imm (Reg r, int32_t imm);
	void		 sub64_imm (Reg r, int32_t imm);
	void		 test8 (Reg a, Reg b);
	/* A call to a host function, through rax */
	void		 call (const void *fn);

	/* The arithmetic group, 32 bits: op r/m32, r32 (the opcodes below);
	 * op r32, r/m32 (the opcode + 2); op r/m32, imm32 (81 /digit) */
	enum Alu {
		ALU_ADD = 0x01, ALU_OR = 0x09, ALU_AND = 0x21, ALU_SUB = 0x29,
		ALU_XOR = 0x31, ALU_CMP = 0x39
	};
	void		 alu_mem_reg (Alu op, Reg base, int32_t disp, Reg r);
	void		 alu_reg_mem (Alu op, Reg r, Reg base, int32_t disp);
	void		 alu_mem_imm (Alu op, Reg base, int32_t disp, int32_t imm);
	/* test r/m32, r32; test r/m32, imm32; test r/m8, imm8 */
	void		 test_mem_reg (Reg base, int32_t disp, Reg r);
	void		 test_mem_imm (Reg base, int32_t disp, uint32_t imm);
	void		 test8_mem_imm (Reg base, int32_t disp, uint8_t imm);
	/* shl and shr r/m32 by a count (1: the D1 form) */
	void		 shl_mem (Reg base, int32_t disp, uint8_t count);
	void		 shr_mem (Reg base, int32_t disp, uint8_t count);
	/* not r32; and r32, imm32 */
	void		 not32 (Reg r);
	void		 and32_imm (Reg r, uint32_t imm);
	/* setcc r8 (al... bl); movzx r32 from a byte or a word in memory or
	 * from a byte register */
	void		 setcc (Cond c, Reg r);
	void		 movzx8_mem (Reg dst, Reg base, int32_t disp);
	void		 movzx16_mem (Reg dst, Reg base, int32_t disp);
	void		 movzx8 (Reg dst, Reg src);

	/* 32-bit register forms: mov, shr by an immediate, cmp and test with
	 * an immediate */
	void		 mov32 (Reg dst, Reg src);
	void		 shr32_imm (Reg r, uint8_t count);
	void		 cmp32_imm (Reg r, uint32_t imm);
	void		 test32_imm (Reg r, uint32_t imm);
	/* 64-bit test r, r; [base + disp] += imm */
	void		 test64 (Reg a, Reg b);
	void		 add64_mem_imm (Reg base, int32_t disp, int32_t imm);
	/* [base + index * scale] (scale 1, 2, 4, 8): 32-bit loads and
	 * stores, a 64-bit load */
	void		 load32_index (Reg dst, Reg base, Reg index, unsigned scale);
	void		 store32_index (Reg base, Reg index, unsigned scale, Reg src);
	void		 load64_index (Reg dst, Reg base, Reg index, unsigned scale);
	/* The other sizes at [base + index * scale]: loads sign extended to
	 * 32 bits (movsx), 64-bit and 8- and 16-bit stores */
	void		 load8s_index (Reg dst, Reg base, Reg index, unsigned scale);
	void		 load16s_index (Reg dst, Reg base, Reg index, unsigned scale);
	void		 store8_index (Reg base, Reg index, unsigned scale, Reg src);
	void		 store16_index (Reg base, Reg index, unsigned scale, Reg src);
	void		 store64_index (Reg base, Reg index, unsigned scale, Reg src);
	/* 64-bit loads and stores at [base + disp]; ror r64, imm; movsx r32
	 * from a byte or a word at [base + disp]; neg r32 */
	void		 load64 (Reg dst, Reg base, int32_t disp);
	void		 store64 (Reg base, int32_t disp, Reg src);
	void		 ror64_imm (Reg r, uint8_t count);
	void		 movsx8_mem (Reg dst, Reg base, int32_t disp);
	void		 movsx16_mem (Reg dst, Reg base, int32_t disp);
	void		 neg32 (Reg r);
	/* bt [base], r: CF = the bit r of the bit string at base */
	void		 bt_mem (Reg base, Reg bit);

	/* Jumps to a label, bound before or after */
	void		 jcc (Cond c, Label &l);
	void		 jmp (Label &l);
	void		 bind (Label &l);

	void		 byte (uint8_t b);
	void		 u32 (uint32_t v);
	void		 u64 (uint64_t v);

private:
	uint8_t		*start, *p, *end;
	bool		 over = false;

	void		 rex (bool w, unsigned reg, unsigned rm, bool force = false);
	/* ModRM (and SIB) for [base + disp32], reg the other operand */
	void		 mem (unsigned reg, Reg base, int32_t disp);
	/* REX, opcode, ModRM, SIB and disp32 0 for [base + index * scale] */
	void		 mem_index (bool w, uint8_t opcode, unsigned reg, Reg base,
			            Reg index, unsigned scale);
	/* The same with a 66 prefix (16 bits) and an opcode after 0F */
	void		 mem_index_ex (bool p66, bool w, bool esc, uint8_t opcode,
				       unsigned reg, Reg base, Reg index, unsigned scale);
	void		 rel32_to (Label &l);
};

} /* namespace x64 */

#endif /* __HL_X64_EMITTER_H__ */
