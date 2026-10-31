/* 
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/vk/games.c, Copyright (C) 2011, 2012, Stefano Teso.
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

#include "hl/core.h"
#include "hl/games.h"
#include "hl/buffer.h"
#include "hl/romset.h"

namespace hl {

static const unsigned current_version = 1;

enum {
	MODE_ALTERNATIVE,
	MODE_INTERLEAVE,
	MODE_CONCATENATE
};

Buffer *
Game::section_data (const char *name)
{
	for (GameSection &section : sections)
		if (!strcmp (section.name, name))
			return section.buffer;
	return NULL;
}

/* A datum is { "name", "size", "crc" (optional), "alias" (optional) } */
static void
get_datum (json_t *datum, const char **name, const char **alias,
           uint32_t *crc, unsigned *size)
{
	const char *crc_text = json_string_value (json_object_get (datum, "crc"));

	*name = json_string_value (json_object_get (datum, "name"));
	*alias = json_string_value (json_object_get (datum, "alias"));
	*size = (unsigned) json_integer_value (json_object_get (datum, "size"));
	*crc = crc_text ? (uint32_t) strtoul (crc_text, NULL, 16) : 0;
}

static Buffer *
load_datum (json_t *datum, RomSet *set)
{
	const char *name, *alias;
	uint32_t crc;
	unsigned size;
	Buffer *buffer;

	get_datum (datum, &name, &alias, &crc, &size);
	if (!name)
		return NULL;

	buffer = set->load (name, alias, crc, size);
	if (!buffer)
		HL_ERROR ("'%s' (%u bytes, CRC %08X) not found", name, size, crc);
	return buffer;
}

static bool
has_datum (json_t *datum, RomSet *set)
{
	const char *name, *alias;
	uint32_t crc;
	unsigned size;

	get_datum (datum, &name, &alias, &crc, &size);
	return name && set->has (name, alias, crc, size);
}

static unsigned
get_section_size (json_t *data, unsigned ndata, unsigned mode)
{
	unsigned i, total_bytes = 0;
	for (i = 0; i < ndata; i++) {
		json_t *datum = json_array_get (data, i);
		json_t *datum_size = json_object_get (datum, "size");
		unsigned nbytes;

		nbytes = json_integer_value (datum_size);
		if (!nbytes)
			return -1;

		switch (mode) {
		case MODE_ALTERNATIVE:
			if (total_bytes == 0)
				total_bytes = nbytes;
			else if (total_bytes != nbytes)
				return 0;
			break;
		case MODE_INTERLEAVE:
		case MODE_CONCATENATE:
			total_bytes += nbytes;
			break;
		}
	}
	return total_bytes;
}

static int
load_section (json_t *root, GameSection *section, RomSet *set)
{
	json_t *name, *type, *data;
	const char *name_value, *type_value;
	unsigned mode, ndata, i;
	uint32_t total_size, base;

	name = json_object_get (root, "name");
	type = json_object_get (root, "type");
	data = json_object_get (root, "data");

	if (!json_is_string (name) ||
	    !json_is_string (type))
		return -1;

	ndata = json_array_size (data);
	if (!ndata)
		return -1;

	name_value = json_string_value (name);
	type_value = json_string_value (type);

	HL_PRINT ("Loading section %s, %s", name_value, type_value);

	if (!strcmp (type_value, "alternative"))
		mode = MODE_ALTERNATIVE;
	else if (!strcmp (type_value, "interleave"))
		mode = MODE_INTERLEAVE;
	else if (!strcmp (type_value, "concatenate"))
		mode = MODE_CONCATENATE;
	else
		return -1;

	total_size = get_section_size (data, ndata, mode);
	if (!total_size)
		return -1;

	strcpy (section->name, name_value);

	switch (mode) {
	case MODE_ALTERNATIVE:
		section->buffer = NULL;
		for (i = 0; i < ndata; i++) {
			json_t *datum = json_array_get (data, i);
			section->buffer = load_datum (datum, set);
			if (section->buffer)
				break;
		}
		if (!section->buffer)
			return -1;
		break;
	case MODE_INTERLEAVE: {
		/* "amnt" bytes of each of "ways" files in turn (2 by default:
		 * the Hikaru's pairs; 4 for Gaelco's NAND chips, MAME's
		 * ROM_LOAD32_BYTE), the next files after them. XXX wrong, we
		 * need to interleave 2 by 2 for hikaru eproms, see braveff */
		json_t *amnt = json_object_get (root, "amnt");
		json_t *ways_json = json_object_get (root, "ways");
		unsigned nbytes = json_integer_value (amnt);
		unsigned ways = ways_json ? json_integer_value (ways_json) : 2;
		if (!amnt)
			return -1;
		if (nbytes != 1 && nbytes != 2 && nbytes != 4 && nbytes != 8)
			return -1;
		if (ways < 2 || ways > 8)
			return -1;
		section->buffer = Buffer::create (total_size, 0);
		if (!section->buffer)
			return -1;
		base = 0;
		for (i = 0; i < ndata; i++) {
			json_t *datum = json_array_get (data, i);
			unsigned j;
			Buffer *buf = load_datum (datum, set);
			if (!buf)
				return -1;
			for (j = 0; j < buf->size (); j += nbytes) {
				unsigned k = base + (j / nbytes) * ways * nbytes + (i % ways) * nbytes;
				uint64_t bytes = buf->get (nbytes, j);
				section->buffer->put (nbytes, k, bytes);
			}
			if (i % ways == ways - 1)
				base += buf->size () * ways;
			hl::destroy (buf);
		}
		break;
	}
	case MODE_CONCATENATE:
		section->buffer = Buffer::create (total_size, 0);
		if (!section->buffer)
			return -1;
		base = 0;
		for (i = 0; i < ndata; i++) {
			json_t *datum = json_array_get (data, i);
			Buffer *buffer = load_datum (datum, set);
			unsigned j;
			if (!buffer)
				return -1;
			for (j = 0; j < buffer->size (); j += 1) {
				uint8_t byte = buffer->get (1, j);
				section->buffer->put (1, j+base, byte);
			}
			base += buffer->size ();
			hl::destroy (buffer);
		}
		break;
	}
	return 0;
}

static int
load_sections (json_t *root, Game *game, RomSet *set, const char *path)
{
	json_t *sections;
	unsigned i, nsections;

	HL_ASSERT (root);
	HL_ASSERT (game);

	sections = json_object_get (root, "sections");

	nsections = json_array_size (sections);
	if (!nsections)
		return -1;

	/* Allocated first, so that the destructor frees a partial load */
	game->sections.resize (nsections, GameSection ());

	for (i = 0; i < nsections; i++) {
		json_t *section = json_array_get (sections, i);
		/* A section from another set ("set": SGNASCAR's drive board
		 * EPROM, Sega Rally 2's), and one the game can do without
		 * ("optional": left empty when missing) */
		const char *other = json_string_value (json_object_get (section, "set"));
		bool optional = json_is_true (json_object_get (section, "optional"));
		RomSet *from = set;
		int ret;

		if (other) {
			const char *names[1] = { other };
			from = new RomSet (path, names, 1);
		}
		ret = load_section (section, &game->sections[i], from);
		if (from != set)
			destroy (from);
		if (ret && optional) {
			const char *name = json_string_value (json_object_get (section, "name"));
			HL_PRINT ("optional section %s not found%s%s, left out",
			          name ? name : "?", other ? " in set " : "", other ? other : "");
			game->sections[i] = GameSection ();
			if (name)
				snprintf (game->sections[i].name, sizeof (game->sections[i].name), "%s", name);
			continue;
		}
		if (ret)
			return -1;
	}
	return 0;
}

static RomSet *
open_romset (GameEntry *entry, const char *path)
{
	const char *names[3] = { entry->name, entry->parent, entry->bios };
	return new RomSet (path, names, NUMELEM (names));
}

Game *
Game::create (GameList *game_list, const char *path, const char *name)
{
	GameEntry *entry;
	RomSet *set;
	Game *game;
	int ret;

	HL_ASSERT (game_list);
	HL_ASSERT (path);
	HL_ASSERT (name);

	entry = game_list->find (name);
	if (!entry)
		return NULL;

	game = new Game ();

	strcpy (game->name, entry->name);
	strcpy (game->mach, entry->mach);
	strcpy (game->parent, entry->parent);
	strcpy (game->audio, entry->audio);

	set = open_romset (entry, path);
	ret = load_sections (entry->root, game, set, path);
	destroy (set);
	if (ret)
		destroy (game);
	return game;
}

GameEntry *
GameList::find (const char *name)
{
	for (GameEntry &entry : entries)
		if (!strcmp (entry.name, name))
			return &entry;
	return NULL;
}

void
GameList::check (unsigned index, const char *path, unsigned *found, unsigned *total)
{
	GameEntry *entry = &entries[index];
	json_t *sections = json_object_get (entry->root, "sections");
	RomSet *set = open_romset (entry, path);
	unsigned i, j;

	*found = *total = 0;
	if (!set)
		return;

	for (i = 0; i < json_array_size (sections); i++) {
		json_t *section = json_array_get (sections, i);
		json_t *data = json_object_get (section, "data");
		const char *type = json_string_value (json_object_get (section, "type"));

		/* What the game can do without is not counted */
		if (json_is_true (json_object_get (section, "optional")))
			continue;
		if (type && !strcmp (type, "alternative")) {
			/* Any one of them will do */
			*total += 1;
			for (j = 0; j < json_array_size (data); j++)
				if (has_datum (json_array_get (data, j), set)) {
					*found += 1;
					break;
				}
		} else {
			for (j = 0; j < json_array_size (data); j++) {
				*total += 1;
				if (has_datum (json_array_get (data, j), set))
					*found += 1;
			}
		}
	}
	destroy (set);
}

/* "Air Trix (Rev A)": the title, and the version in its last parentheses
 * ("Rev A", "original"); "-" without */
static void
split_description (const char *description, char *title, size_t title_size,
                   char *version, size_t version_size)
{
	const char *open = strrchr (description, '(');
	size_t len = strlen (description);

	if (open && len && description[len - 1] == ')') {
		size_t n = (size_t) (open - description);
		while (n && description[n - 1] == ' ')
			n--;
		snprintf (title, title_size, "%.*s", (int) n, description);
		snprintf (version, version_size, "%.*s",
		          (int) (description + len - 1 - (open + 1)), open + 1);
	} else {
		snprintf (title, title_size, "%s", description);
		snprintf (version, version_size, "-");
	}
}

/* A section's files: "name [crc]", joined by sep, a few a line */
static void
print_files (FILE *fp, json_t *data, const char *sep)
{
	size_t j;

	for (j = 0; j < json_array_size (data); j++) {
		json_t *datum = json_array_get (data, j);

		if (j && j % 4 == 0)
			fprintf (fp, "\n%22s", "");
		else if (j)
			fprintf (fp, "%s", sep);
		fprintf (fp, "%s [%s]", json_string_value (json_object_get (datum, "name")),
		         json_string_value (json_object_get (datum, "crc")));
	}
	fprintf (fp, "\n");
}

void
GameList::print (FILE *fp, const char *path)
{
	unsigned i, count = 0;

	for (const GameEntry &entry : entries)
		count += strcmp (entry.name, entry.bios) != 0;
	fprintf (fp, "%u games; their ROMs looked for in %s\n", count, path);
	fprintf (fp, "\n%-10s  %-42s  %-9s  %-8s  %s\n", "set", "title", "version",
	         "cabinet", "year, maker");

	for (i = 0; i < entries.size (); i++) {
		GameEntry *entry = &entries[i];
		json_t *sections = json_object_get (entry->root, "sections");
		char title[128], version[32];
		const char *main_name = NULL, *main_crc = NULL;
		unsigned found, total;
		size_t s;

		/* The BIOS set is the games' requirement, not a game */
		if (!strcmp (entry->name, entry->bios))
			continue;

		split_description (entry->description, title, sizeof (title),
		                   version, sizeof (version));
		fprintf (fp, "\n%-10s  %-42s  %-9s  %-8s  %s  %s\n", entry->name, title,
		         version, entry->link && entry->ffb ? "link+FFB" : entry->link ? "link" :
		         entry->ffb ? "FFB" : "-", entry->year, entry->manufacturer);
		fprintf (fp, "  link:               %s\n", entry->link ?
		         "yes (communication board; [Network] in hikaruloader.ini)" : "no");
		fprintf (fp, "  force feedback:     %s\n", entry->ffb ?
		         "yes (drive board; its EPROM from srally2.zip, simulated without)" : "no");
		fprintf (fp, "  speakers:           %s\n", entry->audio[0] ? entry->audio : "?");

		/* The MAME sets: its own, its parent's, the BIOS' */
		fprintf (fp, "  sets:               %s.zip", entry->name);
		if (entry->parent[0])
			fprintf (fp, ", %s.zip (parent)", entry->parent);
		if (entry->bios[0])
			fprintf (fp, ", %s.zip (BIOS)", entry->bios);
		fprintf (fp, "\n");

		for (s = 0; s < json_array_size (sections); s++) {
			json_t *section = json_array_get (sections, s);
			json_t *data = json_object_get (section, "data");
			const char *name = json_string_value (json_object_get (section, "name"));
			const char *type = json_string_value (json_object_get (section, "type"));
			bool any = type && !strcmp (type, "alternative");

			/* The main ROM: the program, the first EPROM */
			if (name && !strcmp (name, "eprom") && json_array_size (data)) {
				json_t *first = json_array_get (data, 0);
				main_name = json_string_value (json_object_get (first, "name"));
				main_crc = json_string_value (json_object_get (first, "crc"));
			}
			fprintf (fp, "  %-18s  ", any ? "bootrom (one of)" : name);
			print_files (fp, data, any ? " or " : ", ");
		}
		if (main_name)
			fprintf (fp, "  main ROM:           %s, CRC32 %s\n", main_name, main_crc);
		check (i, path, &found, &total);
		fprintf (fp, "  found:              %u of %u%s\n", found, total,
		         found == total ? "" : " (missing ROMs)");
	}
}

Game::~Game ()
{
	for (GameSection &section : sections)
		destroy (section.buffer);
}

/* Copies an optional string member, truncated to the field */
static void
get_string (json_t *rom, const char *key, char *dst, size_t size)
{
	const char *value = json_string_value (json_object_get (rom, key));
	strncpy (dst, value ? value : "", size - 1);
	dst[size - 1] = '\0';
}

static int
parse_rom_list (json_t *roms, GameList *list)
{
	unsigned i, nentries = json_array_size (roms);

	list->entries.resize (nentries, GameEntry ());

	for (i = 0; i < nentries; i++) {
		GameEntry *entry = &list->entries[i];
		json_t *rom = json_array_get (roms, i);
		json_t *name = json_object_get (rom, "name");
		json_t *mach = json_object_get (rom, "mach");

		if (!json_is_string (name) || !json_is_string (mach))
			return -1;

		strncpy (entry->name, json_string_value (name), 31);
		strncpy (entry->mach, json_string_value (mach), 31);
		entry->name[31] = '\0';
		entry->mach[31] = '\0';
		get_string (rom, "parent", entry->parent, sizeof (entry->parent));
		get_string (rom, "bios", entry->bios, sizeof (entry->bios));
		get_string (rom, "description", entry->description, sizeof (entry->description));
		get_string (rom, "year", entry->year, sizeof (entry->year));
		get_string (rom, "manufacturer", entry->manufacturer, sizeof (entry->manufacturer));
		entry->link = json_is_true (json_object_get (rom, "link"));
		entry->ffb = json_is_true (json_object_get (rom, "ffb"));
		get_string (rom, "audio", entry->audio, sizeof (entry->audio));
		if (!entry->description[0])
			strcpy (entry->description, entry->name);
		entry->root = rom;

		HL_LOG ("GAME #%u = { '%s', '%s', %p }\n",
		        i, entry->name, entry->mach, entry->root);
	}
	return 0;
}

GameList *
GameList::load (const char *path)
{
	GameList *list = new GameList ();
	Buffer *buf = NULL;
	json_t *root, *version, *roms;
	json_error_t error;
	void *text;

	HL_ASSERT (path);

	if (!list)
		return NULL;

	buf = Buffer::from_file (path, ~0);
	if (!buf) {
		HL_ERROR ("could not open game list '%s'", path);
		goto fail;
	}

	text = buf->ptr (0);

	/* The file contents are not NUL-terminated */
	root = json_loadb ((const char *) text, buf->size (), 0, &error);
	if (!root) {
		HL_ERROR ("can't load game list '%s'", error.text);
		goto fail;
	}
	hl::destroy (buf);

	version = json_object_get (root, "version");
	if (json_is_integer (version)) {
		json_int_t value = json_integer_value (version);
		if (current_version != (unsigned) value) {
			HL_ERROR ("invalid game list version: found %u, expected %u",
			          (unsigned) value, current_version);
			goto fail;
		}
	} else {
		HL_ERROR ("not game list version");
		goto fail;
	}

	roms = json_object_get (root, "roms");
	if (!json_is_array (roms)) {
		HL_ERROR ("invalid game list format");
		goto fail;
	}

	if (parse_rom_list (roms, list)) {
		HL_ERROR ("failed to parse rom list");
		goto fail;
	}

	/* The entries point into it */
	list->root = root;
	return list;
fail:
	destroy (list);
	hl::destroy (buf);
	return NULL;
}

GameList::~GameList ()
{
	json_decref (root);
}

} /* namespace hl */
