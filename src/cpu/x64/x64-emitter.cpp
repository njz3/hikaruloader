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

/* x86-64 machine code; see x64-emitter.h. The encodings follow the Intel
 * SDM, volume 2: a REX prefix (0100WRXB) when a register is r8-r15 or the
 * operation 64-bit, the opcode, a ModRM byte, a SIB byte when the base is
 * rsp or r12, the displacement, the immediate. */

#include "cpu/x64/x64-emitter.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace x64 {

/* CodeArena */

CodeArena::CodeArena (size_t size_) : size (size_), used (0)
{
#ifdef _WIN32
	mem = (uint8_t *) VirtualAlloc (NULL, size, MEM_COMMIT | MEM_RESERVE,
	                                PAGE_EXECUTE_READWRITE);
#else
	mem = (uint8_t *) mmap (NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
	                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mem == (uint8_t *) MAP_FAILED)
		mem = NULL;
#endif
	if (!mem)
		size = 0;
}

CodeArena::~CodeArena ()
{
	if (!mem)
		return;
#ifdef _WIN32
	VirtualFree (mem, 0, MEM_RELEASE);
#else
	munmap (mem, size);
#endif
}

uint8_t *
CodeArena::take (size_t n, size_t align)
{
	size_t at = (used + align - 1) & ~(align - 1);

	if (at + n > size)
		return NULL;
	used = at + n;
	return mem + at;
}

/* Emitter */

void
Emitter::byte (uint8_t b)
{
	if (p < end)
		*p++ = b;
	else
		over = true;
}

void
Emitter::u32 (uint32_t v)
{
	for (unsigned i = 0; i < 4; i++)
		byte ((uint8_t) (v >> (8 * i)));
}

void
Emitter::u64 (uint64_t v)
{
	for (unsigned i = 0; i < 8; i++)
		byte ((uint8_t) (v >> (8 * i)));
}

void
Emitter::rex (bool w, unsigned reg, unsigned rm, bool force)
{
	uint8_t r = 0x40 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0) | ((rm & 8) ? 1 : 0);

	if (r != 0x40 || force)
		byte (r);
}

void
Emitter::mem (unsigned reg, Reg base, int32_t disp)
{
	/* mod 10: disp32; rm 100 asks for a SIB, which rsp and r12 need */
	byte ((uint8_t) (0x80 | ((reg & 7) << 3) | (base & 7)));
	if ((base & 7) == RSP)
		byte (0x24);
	u32 ((uint32_t) disp);
}

void
Emitter::push (Reg r)
{
	rex (false, 0, r);
	byte ((uint8_t) (0x50 | (r & 7)));
}

void
Emitter::pop (Reg r)
{
	rex (false, 0, r);
	byte ((uint8_t) (0x58 | (r & 7)));
}

void
Emitter::mov (Reg dst, Reg src)
{
	/* 89 /r: mov r/m64, r64 */
	rex (true, src, dst);
	byte (0x89);
	byte ((uint8_t) (0xC0 | ((src & 7) << 3) | (dst & 7)));
}

void
Emitter::mov_imm64 (Reg dst, uint64_t imm)
{
	/* REX.W B8+r io */
	rex (true, 0, dst);
	byte ((uint8_t) (0xB8 | (dst & 7)));
	u64 (imm);
}

void
Emitter::mov_imm32 (Reg dst, uint32_t imm)
{
	/* B8+r id */
	rex (false, 0, dst);
	byte ((uint8_t) (0xB8 | (dst & 7)));
	u32 (imm);
}

void
Emitter::lea32 (Reg dst, Reg src, int32_t disp)
{
	/* 8D /r: lea r32, m */
	rex (false, dst, src);
	byte (0x8D);
	mem (dst, src, disp);
}

void
Emitter::load32 (Reg dst, Reg base, int32_t disp)
{
	/* 8B /r: mov r32, r/m32 */
	rex (false, dst, base);
	byte (0x8B);
	mem (dst, base, disp);
}

void
Emitter::store32 (Reg base, int32_t disp, Reg src)
{
	/* 89 /r: mov r/m32, r32 */
	rex (false, src, base);
	byte (0x89);
	mem (src, base, disp);
}

void
Emitter::store32_imm (Reg base, int32_t disp, uint32_t imm)
{
	/* C7 /0 id: mov r/m32, imm32 */
	rex (false, 0, base);
	byte (0xC7);
	mem (0, base, disp);
	u32 (imm);
}

void
Emitter::add32_mem_imm (Reg base, int32_t disp, int32_t imm)
{
	/* 81 /0 id: add r/m32, imm32 */
	rex (false, 0, base);
	byte (0x81);
	mem (0, base, disp);
	u32 ((uint32_t) imm);
}

void
Emitter::cmp32_mem (Reg base, int32_t disp, Reg r)
{
	/* 39 /r: cmp r/m32, r32 */
	rex (false, r, base);
	byte (0x39);
	mem (r, base, disp);
}

void
Emitter::cmp8_mem_imm (Reg base, int32_t disp, uint8_t imm)
{
	/* 80 /7 ib: cmp r/m8, imm8 */
	rex (false, 0, base);
	byte (0x80);
	mem (7, base, disp);
	byte (imm);
}

void
Emitter::add64_imm (Reg r, int32_t imm)
{
	/* REX.W 81 /0 id */
	rex (true, 0, r);
	byte (0x81);
	byte ((uint8_t) (0xC0 | (r & 7)));
	u32 ((uint32_t) imm);
}

void
Emitter::sub64_imm (Reg r, int32_t imm)
{
	/* REX.W 81 /5 id */
	rex (true, 0, r);
	byte (0x81);
	byte ((uint8_t) (0xE8 | (r & 7)));
	u32 ((uint32_t) imm);
}

void
Emitter::test8 (Reg a, Reg b)
{
	/* 84 /r: test r/m8, r8; a REX for sil, dil... */
	rex (false, b, a, (a & 7) >= 4 || (b & 7) >= 4);
	byte (0x84);
	byte ((uint8_t) (0xC0 | ((b & 7) << 3) | (a & 7)));
}

void
Emitter::call (const void *fn)
{
	/* mov rax, imm64; call rax (FF /2) */
	mov_imm64 (RAX, (uint64_t) (uintptr_t) fn);
	byte (0xFF);
	byte (0xD0);
}

void
Emitter::alu_mem_reg (Alu op, Reg base, int32_t disp, Reg r)
{
	rex (false, r, base);
	byte ((uint8_t) op);
	mem (r, base, disp);
}

void
Emitter::alu_reg_mem (Alu op, Reg r, Reg base, int32_t disp)
{
	rex (false, r, base);
	byte ((uint8_t) (op + 2));
	mem (r, base, disp);
}

void
Emitter::alu_mem_imm (Alu op, Reg base, int32_t disp, int32_t imm)
{
	/* The /digit is the opcode's bits 5-3: add 0, or 1, and 4, sub 5,
	 * xor 6, cmp 7 */
	rex (false, 0, base);
	byte (0x81);
	mem ((unsigned) (op >> 3), base, disp);
	u32 ((uint32_t) imm);
}

void
Emitter::test_mem_reg (Reg base, int32_t disp, Reg r)
{
	/* 85 /r */
	rex (false, r, base);
	byte (0x85);
	mem (r, base, disp);
}

void
Emitter::test_mem_imm (Reg base, int32_t disp, uint32_t imm)
{
	/* F7 /0 id */
	rex (false, 0, base);
	byte (0xF7);
	mem (0, base, disp);
	u32 (imm);
}

void
Emitter::test8_mem_imm (Reg base, int32_t disp, uint8_t imm)
{
	/* F6 /0 ib */
	rex (false, 0, base);
	byte (0xF6);
	mem (0, base, disp);
	byte (imm);
}

void
Emitter::shl_mem (Reg base, int32_t disp, uint8_t count)
{
	/* D1 /4, C1 /4 ib */
	rex (false, 0, base);
	byte (count == 1 ? 0xD1 : 0xC1);
	mem (4, base, disp);
	if (count != 1)
		byte (count);
}

void
Emitter::shr_mem (Reg base, int32_t disp, uint8_t count)
{
	/* D1 /5, C1 /5 ib */
	rex (false, 0, base);
	byte (count == 1 ? 0xD1 : 0xC1);
	mem (5, base, disp);
	if (count != 1)
		byte (count);
}

void
Emitter::not32 (Reg r)
{
	/* F7 /2 */
	rex (false, 0, r);
	byte (0xF7);
	byte ((uint8_t) (0xD0 | (r & 7)));
}

void
Emitter::and32_imm (Reg r, uint32_t imm)
{
	/* 81 /4 id */
	rex (false, 0, r);
	byte (0x81);
	byte ((uint8_t) (0xE0 | (r & 7)));
	u32 (imm);
}

void
Emitter::setcc (Cond c, Reg r)
{
	/* 0F 90+cc /0; a REX for spl, bpl, sil, dil */
	rex (false, 0, r, (r & 7) >= 4);
	byte (0x0F);
	byte ((uint8_t) (0x90 | c));
	byte ((uint8_t) (0xC0 | (r & 7)));
}

void
Emitter::movzx8_mem (Reg dst, Reg base, int32_t disp)
{
	/* 0F B6 /r */
	rex (false, dst, base);
	byte (0x0F);
	byte (0xB6);
	mem (dst, base, disp);
}

void
Emitter::movzx16_mem (Reg dst, Reg base, int32_t disp)
{
	/* 0F B7 /r */
	rex (false, dst, base);
	byte (0x0F);
	byte (0xB7);
	mem (dst, base, disp);
}

void
Emitter::movzx8 (Reg dst, Reg src)
{
	/* 0F B6 /r, register form */
	rex (false, dst, src, (src & 7) >= 4);
	byte (0x0F);
	byte (0xB6);
	byte ((uint8_t) (0xC0 | ((dst & 7) << 3) | (src & 7)));
}

void
Emitter::mov32 (Reg dst, Reg src)
{
	/* 89 /r */
	rex (false, src, dst);
	byte (0x89);
	byte ((uint8_t) (0xC0 | ((src & 7) << 3) | (dst & 7)));
}

void
Emitter::shr32_imm (Reg r, uint8_t count)
{
	/* C1 /5 ib */
	rex (false, 0, r);
	byte (0xC1);
	byte ((uint8_t) (0xE8 | (r & 7)));
	byte (count);
}

void
Emitter::cmp32_imm (Reg r, uint32_t imm)
{
	/* 81 /7 id */
	rex (false, 0, r);
	byte (0x81);
	byte ((uint8_t) (0xF8 | (r & 7)));
	u32 (imm);
}

void
Emitter::test32_imm (Reg r, uint32_t imm)
{
	/* F7 /0 id */
	rex (false, 0, r);
	byte (0xF7);
	byte ((uint8_t) (0xC0 | (r & 7)));
	u32 (imm);
}

void
Emitter::test64 (Reg a, Reg b)
{
	/* REX.W 85 /r */
	rex (true, b, a);
	byte (0x85);
	byte ((uint8_t) (0xC0 | ((b & 7) << 3) | (a & 7)));
}

void
Emitter::add64_mem_imm (Reg base, int32_t disp, int32_t imm)
{
	/* REX.W 81 /0 id */
	rex (true, 0, base);
	byte (0x81);
	mem (0, base, disp);
	u32 ((uint32_t) imm);
}

void
Emitter::mem_index (bool w, uint8_t opcode, unsigned reg, Reg base, Reg index,
                    unsigned scale)
{
	static const uint8_t ss[9] = { 0, 0, 1, 0, 2, 0, 0, 0, 3 };

	/* REX: W, R from reg, X from index, B from base */
	uint8_t r = 0x40 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0) |
	            ((index & 8) ? 2 : 0) | ((base & 8) ? 1 : 0);
	if (r != 0x40)
		byte (r);
	byte (opcode);
	/* mod 10, rm 100: a SIB, then disp32 */
	byte ((uint8_t) (0x84 | ((reg & 7) << 3)));
	byte ((uint8_t) ((ss[scale] << 6) | ((index & 7) << 3) | (base & 7)));
	u32 (0);
}

void
Emitter::load32_index (Reg dst, Reg base, Reg index, unsigned scale)
{
	mem_index (false, 0x8B, dst, base, index, scale);
}

void
Emitter::store32_index (Reg base, Reg index, unsigned scale, Reg src)
{
	mem_index (false, 0x89, src, base, index, scale);
}

void
Emitter::load64_index (Reg dst, Reg base, Reg index, unsigned scale)
{
	mem_index (true, 0x8B, dst, base, index, scale);
}

void
Emitter::bt_mem (Reg base, Reg bit)
{
	/* 0F A3 /r: bt r/m32, r32, a bit string in memory */
	rex (false, bit, base);
	byte (0x0F);
	byte (0xA3);
	mem (bit, base, 0);
}

void
Emitter::rel32_to (Label &l)
{
	if (l.target) {
		u32 ((uint32_t) (l.target - (p + 4)));
	} else {
		l.waiting.push_back (p);
		u32 (0);
	}
}

void
Emitter::jcc (Cond c, Label &l)
{
	/* 0F 80+cc cd */
	byte (0x0F);
	byte ((uint8_t) (0x80 | c));
	rel32_to (l);
}

void
Emitter::jmp (Label &l)
{
	/* E9 cd */
	byte (0xE9);
	rel32_to (l);
}

void
Emitter::bind (Label &l)
{
	l.target = p;
	for (uint8_t *at : l.waiting)
		if (at + 4 <= end) {
			uint32_t rel = (uint32_t) (l.target - (at + 4));
			memcpy (at, &rel, 4);
		}
	l.waiting.clear ();
}

} /* namespace x64 */
