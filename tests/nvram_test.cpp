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

/* The nvram files' tests: their layout, blocks in and out, the files refused.
 * Run by ctest, or alone: nvramtest. */

#include <stdio.h>
#include <string.h>

#include <filesystem>
#include <vector>

#include "hl/nvram.h"

static int failures, checks;

#define CHECK(name_, got_, want_) \
	do { \
		unsigned g_ = (unsigned) (got_), w_ = (unsigned) (want_); \
		checks++; \
		if (g_ != w_) { \
			failures++; \
			fprintf (stderr, "FAIL %s: %s = %X, expected %X\n", name_, #got_, g_, w_); \
		} \
	} while (0)

#define DIR	"nvramtest.dir"
#define PATH	DIR "/game.nvram"

static std::vector<uint8_t>
pattern (size_t size, unsigned seed)
{
	std::vector<uint8_t> data (size);

	for (size_t i = 0; i < size; i++)
		data[i] = (uint8_t) (i * 7 + seed + (i >> 8));
	return data;
}

static std::vector<uint8_t>
read_file (const char *path)
{
	std::vector<uint8_t> data;
	FILE *fp = fopen (path, "rb");
	int c;

	if (!fp)
		return data;
	while ((c = fgetc (fp)) != EOF)
		data.push_back ((uint8_t) c);
	fclose (fp);
	return data;
}

static void
write_file (const char *path, const std::vector<uint8_t> &data)
{
	FILE *fp = fopen (path, "wb");

	fwrite (data.data (), 1, data.size (), fp);
	fclose (fp);
}

static uint32_t
u32 (const std::vector<uint8_t> &d, size_t offs)
{
	return d[offs] | (d[offs + 1] << 8) | (d[offs + 2] << 16) | ((uint32_t) d[offs + 3] << 24);
}

static void
put_u32 (std::vector<uint8_t> &d, size_t offs, uint32_t val)
{
	for (unsigned i = 0; i < 4; i++)
		d[offs + i] = (uint8_t) (val >> (i * 8));
}

static bool
same (const std::vector<uint8_t> *got, const std::vector<uint8_t> &want)
{
	return got && *got == want;
}

/* The file as the doc in hl/nvram.h has it */
static void
test_layout (void)
{
	std::vector<uint8_t> eeprom = pattern (128, 1), bram = pattern (65536, 2), f;
	hl::Nvram nv;
	unsigned i;

	nv.set ("EEPR", eeprom.data (), eeprom.size ());
	nv.set ("BRAM", bram.data (), bram.size ());
	CHECK ("save", nv.save (PATH), 1);
	CHECK ("the directory made", std::filesystem::is_directory (DIR), 1);
	CHECK ("no temporary left", std::filesystem::exists (PATH ".tmp"), 0);

	f = read_file (PATH);
	CHECK ("magic", memcmp (f.data (), "HLNV", 4), 0);
	CHECK ("version", u32 (f, 4), 1);
	CHECK ("count", u32 (f, 8), 2);
	CHECK ("header reserved", u32 (f, 12), 0);
	CHECK ("1st type", memcmp (&f[16], "EEPR", 4), 0);
	CHECK ("1st offset", u32 (f, 20), 48);
	CHECK ("1st size", u32 (f, 24), 128);
	CHECK ("1st reserved", u32 (f, 28), 0);
	CHECK ("2nd type", memcmp (&f[32], "BRAM", 4), 0);
	CHECK ("2nd offset", u32 (f, 36), 48 + 128);
	CHECK ("2nd size", u32 (f, 40), 65536);
	CHECK ("1st data", memcmp (&f[48], eeprom.data (), 128), 0);
	CHECK ("2nd data", memcmp (&f[176], bram.data (), 65536), 0);
	CHECK ("file size", f.size (), 176 + 65536);

	/* An odd size, padded to 16 */
	nv.set ("ODD!", "abc", 3);
	CHECK ("save odd", nv.save (PATH), 1);
	f = read_file (PATH);
	CHECK ("odd count", u32 (f, 8), 3);
	CHECK ("data after 3 entries", u32 (f, 20), 64);
	CHECK ("odd offset", u32 (f, 52), 64 + 128 + 65536);
	CHECK ("odd size", u32 (f, 56), 3);
	CHECK ("padded", f.size (), 64 + 128 + 65536 + 16);
	for (i = 3; i < 16; i++)
		CHECK ("padding zero", f[64 + 128 + 65536 + i], 0);
}

static void
test_round_trip (void)
{
	std::vector<uint8_t> eeprom = pattern (128, 3), bram = pattern (65536, 4);
	hl::Nvram nv, back;

	nv.set ("EEPR", eeprom.data (), eeprom.size ());
	nv.set ("BRAM", bram.data (), bram.size ());
	CHECK ("save", nv.save (PATH), 1);
	CHECK ("load", back.load (PATH), hl::Nvram::LOADED);
	CHECK ("count", back.count (), 2);
	CHECK ("EEPROM back", same (back.get ("EEPR"), eeprom), 1);
	CHECK ("BRAM back", same (back.get ("BRAM"), bram), 1);
	CHECK ("no such block", back.get ("NONE") == NULL, 1);

	/* A set replaces, a save over an old file replaces it */
	eeprom = pattern (128, 5);
	back.set ("EEPR", eeprom.data (), eeprom.size ());
	CHECK ("count after a replace", back.count (), 2);
	CHECK ("save again", back.save (PATH), 1);
	CHECK ("load again", nv.load (PATH), hl::Nvram::LOADED);
	CHECK ("EEPROM replaced", same (nv.get ("EEPR"), eeprom), 1);
	CHECK ("BRAM kept", same (nv.get ("BRAM"), bram), 1);

	/* No blocks at all */
	{
		hl::Nvram empty, other;

		CHECK ("save empty", empty.save (PATH), 1);
		CHECK ("empty's size", read_file (PATH).size (), 16);
		CHECK ("load empty", other.load (PATH), hl::Nvram::LOADED);
		CHECK ("empty's count", other.count (), 0);
	}
}

/* The blocks of the types the machine doesn't know, written back */
static void
test_unknown_kept (void)
{
	std::vector<uint8_t> extra = pattern (40, 6), eeprom = pattern (128, 7);
	hl::Nvram nv, back;

	nv.set ("XTRA", extra.data (), extra.size ());
	CHECK ("save", nv.save (PATH), 1);
	CHECK ("load", back.load (PATH), hl::Nvram::LOADED);
	back.set ("EEPR", eeprom.data (), eeprom.size ());
	CHECK ("save back", back.save (PATH), 1);
	CHECK ("load back", nv.load (PATH), hl::Nvram::LOADED);
	CHECK ("unknown kept", same (nv.get ("XTRA"), extra), 1);
	CHECK ("EEPROM added", same (nv.get ("EEPR"), eeprom), 1);
}

/* Files refused, the blocks held left as they were */
static void
test_refused (void)
{
	std::vector<uint8_t> eeprom = pattern (128, 8), good, bad;
	hl::Nvram nv, held;

	held.set ("EEPR", eeprom.data (), eeprom.size ());
	CHECK ("missing", held.load (DIR "/missing.nvram"), hl::Nvram::MISSING);

	nv.set ("EEPR", eeprom.data (), eeprom.size ());
	CHECK ("save", nv.save (PATH), 1);
	good = read_file (PATH);

	bad = good;
	bad[0] = 'X';
	write_file (PATH, bad);
	CHECK ("magic", held.load (PATH), hl::Nvram::INVALID);

	bad = good;
	put_u32 (bad, 4, 2);
	write_file (PATH, bad);
	CHECK ("later version", held.load (PATH), hl::Nvram::INVALID);

	bad = good;
	put_u32 (bad, 8, 1000);
	write_file (PATH, bad);
	CHECK ("too many blocks", held.load (PATH), hl::Nvram::INVALID);

	bad = good;
	bad.resize (bad.size () - 1);
	write_file (PATH, bad);
	CHECK ("truncated data", held.load (PATH), hl::Nvram::INVALID);

	bad = good;
	put_u32 (bad, 20, 0xFFFFFFF0);
	write_file (PATH, bad);
	CHECK ("offset past the end", held.load (PATH), hl::Nvram::INVALID);

	bad.assign (good.begin (), good.begin () + 8);
	write_file (PATH, bad);
	CHECK ("no header", held.load (PATH), hl::Nvram::INVALID);

	write_file (PATH, std::vector<uint8_t> ());
	CHECK ("empty file", held.load (PATH), hl::Nvram::INVALID);

	CHECK ("held after the refusals", same (held.get ("EEPR"), eeprom), 1);
	CHECK ("held count", held.count (), 1);
}

int
main (int argc, char **argv)
{
	std::error_code error;

	(void) argc;
	(void) argv;
	std::filesystem::remove_all (DIR, error);

	test_layout ();
	test_round_trip ();
	test_unknown_kept ();
	test_refused ();

	std::filesystem::remove_all (DIR, error);
	printf ("nvramtest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
