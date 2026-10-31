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

#ifndef __HL_HIKARU_AICA_MATH_H__
#define __HL_HIKARU_AICA_MATH_H__

#include <stdint.h>

/* The AICA's arithmetic that needs no machine: the DSP's ring buffer floats,
 * the ADPCM decoder's step. hikaru-aica.cpp uses them; tests/aica_test.cpp
 * checks them. */

namespace hikaru {

static inline int32_t
aica_sext (int32_t v, unsigned bits)
{
	return (int32_t) ((uint32_t) v << (32 - bits)) >> (32 - bits);
}

static inline int32_t
aica_clamp (int32_t v, int32_t lo, int32_t hi)
{
	return (v < lo) ? lo : (v > hi) ? hi : v;
}

/* The ring buffer's 16-bit float, from a 24-bit value: sign, four bits of
 * exponent (the leading bits like the sign), eleven of mantissa */
static inline uint16_t
aica_dsp_pack (int32_t val)
{
	uint32_t sign = (val >> 23) & 1, temp = ((uint32_t) val ^ ((uint32_t) val << 1)) & 0xFFFFFF;
	uint32_t exponent = 0;

	while (!(temp & 0x800000) && exponent < 12) {
		exponent++;
		temp <<= 1;
	}
	if (exponent < 12)
		val = (int32_t) (((uint32_t) val << exponent) & 0x3FFFFF);
	else
		val = (int32_t) ((uint32_t) val << 11);
	val >>= 11;
	return (uint16_t) ((val & 0x7FF) | (sign << 15) | (exponent << 11));
}

/* And back, to 24 bits */
static inline int32_t
aica_dsp_unpack (uint16_t val)
{
	uint32_t sign = (val >> 15) & 1, exponent = (val >> 11) & 0xF;
	uint32_t uval = (uint32_t) (val & 0x7FF) << 11;

	/* The implied bit, unlike the sign; none past 11, the mantissa being
	 * the value's low bits */
	if (exponent > 11) {
		exponent = 11;
		uval |= sign << 22;
	} else
		uval |= (sign ^ 1) << 22;
	uval |= sign << 23;
	return aica_sext ((int32_t) uval, 24) >> exponent;
}

/* Yamaha's ADPCM: one nibble decoded, value and step updated; the value
 * stays in 16 bits, the step in 7F-6000 */
static inline int32_t
aica_adpcm_step (int32_t &value, int32_t &step, unsigned nib)
{
	/* The step scaled by each nibble's magnitude */
	static const int32_t scale[8] = { 230, 230, 230, 230, 307, 409, 512, 614 };
	int32_t diff = (step * (int32_t) ((nib & 7) * 2 + 1)) >> 3;

	value += (nib & 8) ? -diff : diff;
	value = aica_clamp (value, -32768, 32767);
	step = aica_clamp ((step * scale[nib & 7]) >> 8, 0x7F, 0x6000);
	return value;
}

} /* namespace hikaru */

#endif /* __HL_HIKARU_AICA_MATH_H__ */
