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

/* The common roles of the games' own controls (games.json "controls"): the
 * row of controls.ini each reads, the same in all the games that have it */
static const GameRole roles[] = {
	{ "Steering",	"Driving",	"P1_Steer",	GameInput::AXIS },
	{ "Accelerator", "Driving",	"P1_Gas",	GameInput::PEDAL },
	{ "Brake",	"Driving",	"P1_Brake",	GameInput::PEDAL },
	{ "ViewChange",	"Driving",	"ViewChange",	GameInput::SWITCH },
	{ "ShiftUp",	"Driving",	"GearUp",	GameInput::SWITCH },
	{ "ShiftDown",	"Driving",	"GearDown",	GameInput::SWITCH },
	{ "Elevator",	"Flight",	"P1_Elevator",	GameInput::AXIS },
	{ "Aileron",	"Flight",	"P1_Aileron",	GameInput::AXIS },
	{ "Rudder",	"Flight",	"P1_Rudder",	GameInput::AXIS },
	{ "LeverL",	"Flight",	"P1_ThrustL",	GameInput::LEVER },
	{ "LeverR",	"Flight",	"P1_ThrustR",	GameInput::LEVER },
	{ "P1_GunX",	"Pointer",	"P1_AimX",	GameInput::LEVER },
	{ "P1_GunY",	"Pointer",	"P1_AimY",	GameInput::LEVER },
	{ "P2_GunX",	"Pointer",	"P2_AimX",	GameInput::LEVER },
	{ "P2_GunY",	"Pointer",	"P2_AimY",	GameInput::LEVER },
	{ "P1_Trigger",	"Pointer",	"P1_Water",	GameInput::SWITCH },
	{ "P1_Reload",	"Pointer",	"P1_OffScreen",	GameInput::SWITCH },
	{ "P2_Trigger",	"Pointer",	"P2_Water",	GameInput::SWITCH },
	{ "P2_Reload",	"Pointer",	"P2_OffScreen",	GameInput::SWITCH },
	{ "P1_StickX",	"Pointer",	"P1_StickX",	GameInput::AXIS },
	{ "P1_StickY",	"Pointer",	"P1_StickY",	GameInput::AXIS },
	{ "P2_StickX",	"Pointer",	"P2_StickX",	GameInput::AXIS },
	{ "P2_StickY",	"Pointer",	"P2_StickY",	GameInput::AXIS },
};

const GameRole *
game_role (const char *role)
{
	for (const GameRole &r : roles)
		if (!strcmp (r.role, role))
			return &r;
	return NULL;
}

static const unsigned current_version = 1;

enum {
	MODE_ALTERNATIVE,
	MODE_INTERLEAVE,
	MODE_CONCATENATE,
	MODE_PLACE
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
		case MODE_PLACE:
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
	else if (!strcmp (type_value, "place"))
		mode = MODE_PLACE;
	else
		return -1;

	total_size = (mode == MODE_PLACE) ? (uint32_t) json_integer_value (json_object_get (root, "size")) :
	             get_section_size (data, ndata, mode);
	if (!total_size)
		return -1;

	strcpy (section->name, name_value);

	switch (mode) {
	case MODE_ALTERNATIVE:
		section->buffer = NULL;
		for (i = 0; i < ndata; i++) {
			json_t *datum = json_array_get (data, i);
			section->buffer = load_datum (datum, set);
			if (section->buffer) {
				section->chosen = (int) i;
				break;
			}
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
	case MODE_PLACE: {
		/* A region with its files at given offsets, as MAME's ROM_LOAD
		 * ("step": 4 for its ROM_LOAD32_WORD: 16 bits of each 32), then
		 * the region's parts it copies ("copy") */
		json_t *copies = json_object_get (root, "copy");
		section->buffer = Buffer::create (total_size, 0);
		if (!section->buffer)
			return -1;
		memset (section->buffer->ptr (0), (int) json_integer_value (json_object_get (root, "fill")), total_size);
		for (i = 0; i < ndata; i++) {
			json_t *datum = json_array_get (data, i);
			uint32_t offset = (uint32_t) json_integer_value (json_object_get (datum, "offset"));
			unsigned step = (unsigned) json_integer_value (json_object_get (datum, "step"));
			Buffer *buf = load_datum (datum, set);
			unsigned j;
			if (!buf)
				return -1;
			if (step == 4) {
				for (j = 0; j + 2 <= buf->size () && offset + j * 2 + 2 <= total_size; j += 2)
					section->buffer->put (2, offset + j * 2, buf->get (2, j));
			} else if (offset + buf->size () <= total_size)
				memcpy (section->buffer->ptr (offset), buf->ptr (0), buf->size ());
			hl::destroy (buf);
		}
		for (i = 0; i < json_array_size (copies); i++) {
			json_t *c = json_array_get (copies, i);
			uint32_t src = (uint32_t) json_integer_value (json_object_get (c, "src"));
			uint32_t dst = (uint32_t) json_integer_value (json_object_get (c, "dst"));
			uint32_t n = (uint32_t) json_integer_value (json_object_get (c, "size"));
			if (src + n <= total_size && dst + n <= total_size)
				memmove (section->buffer->ptr (dst), section->buffer->ptr (src), n);
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
open_romset (GameEntry *entry, const char *path, const char *archive = NULL)
{
	const char *names[3] = { entry->name, entry->parent, entry->bios };
	return new RomSet (path, names, NUMELEM (names), archive);
}

Game *
Game::create (GameList *game_list, const char *path, const char *name, const char *archive)
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
	{
		const char *text;

		if ((text = json_string_value (json_object_get (entry->root, "board"))))
			snprintf (game->board, sizeof (game->board), "%s", text);
		if ((text = json_string_value (json_object_get (entry->root, "key"))))
			game->key = (uint32_t) strtoul (text, NULL, 16);
		if ((text = json_string_value (json_object_get (entry->root, "id"))))
			game->id = (uint32_t) strtoul (text, NULL, 16);
		if ((text = json_string_value (json_object_get (entry->root, "gdrom"))))
			snprintf (game->gdrom, sizeof (game->gdrom), "%s", text);
		if ((text = json_string_value (json_object_get (entry->root, "g2_board"))))
			snprintf (game->g2_board, sizeof (game->g2_board), "%s", text);
		game->naomi2 = json_is_true (json_object_get (entry->root, "naomi2"));
		game->boards = (unsigned) json_integer_value (json_object_get (entry->root, "boards"));
		game->network_boards = (unsigned) json_integer_value (json_object_get (entry->root, "network_boards"));
		game->net_dimm = json_is_true (json_object_get (entry->root, "net_dimm"));
		game->motion_pc = json_is_true (json_object_get (entry->root, "motion_pc"));
		{
			json_t *io_boards = json_object_get (entry->root, "io_boards");

			for (size_t k = 0; k < json_array_size (io_boards); k++)
				game->io_boards.push_back (json_string_value (json_array_get (io_boards, k)));
		}
		{
			/* The clones read their parent's cabinet */
			json_t *controls = json_object_get (entry->root, "controls");
			const char *role;
			json_t *at;

			if (!controls && entry->parent[0]) {
				const GameEntry *parent = game_list->find (entry->parent);

				if (parent)
					controls = json_object_get (parent->root, "controls");
			}
			json_object_foreach (controls, role, at) {
				const GameRole *r = game_role (role);
				const char *where = json_is_object (at) ?
				                    json_string_value (json_object_get (at, "at")) :
				                    json_string_value (at);
				const char *label = json_is_object (at) ?
				                    json_string_value (json_object_get (at, "label")) : NULL;
				unsigned player, n;
				GameInput in;

				if (!r || !where) {
					HL_PRINT ("games: %s: control '%s' unknown", game->name, role);
					continue;
				}
				in.role = role;
				in.section = r->section;
				in.key = r->key;
				in.label = label ? label : role;
				in.kind = r->kind;
				if (sscanf (where, "an:%u", &n) == 1 && n < 8 && r->kind != GameInput::SWITCH) {
					if (game->analogs.size () <= n)
						game->analogs.resize (n + 1);
					game->analogs[n] = in;
				} else if (sscanf (where, "p%u:%u", &player, &n) == 2 && player >= 1 && player <= 2 &&
				           n < 16 && r->kind == GameInput::SWITCH) {
					in.player = player - 1;
					in.mask = (uint16_t) (1u << n);
					game->switches.push_back (in);
				} else
					HL_PRINT ("games: %s: control '%s' at '%s' not understood", game->name, role, where);
			}
		}
		{
			json_t *screens = json_object_get (entry->root, "screens");

			for (size_t k = 0; k < json_array_size (screens) && k < 4; k++)
				game->screens[game->num_screens++] = (unsigned) json_integer_value (json_array_get (screens, k));
		}
	}

	game->entry = entry;
	game->path = path;
	game->archive = archive ? archive : "";
	set = open_romset (entry, path, archive);
	ret = load_sections (entry->root, game, set, path);
	destroy (set);
	if (ret)
		destroy (game);
	return game;
}

/* A file's "region", as the bit of a game's header */
static unsigned
region_bit (json_t *datum)
{
	static const char letters[] = "JUEKA";
	const char *text = json_string_value (json_object_get (datum, "region"));
	const char *at = text && text[0] ? strchr (letters, text[0]) : NULL;

	return at ? 1u << (at - letters) : 0;
}

bool
Game::use_region (const char *section_name, unsigned mask)
{
	json_t *list = json_object_get (entry->root, "sections");
	bool changed = false;
	size_t s;

	for (s = 0; s < json_array_size (list) && s < sections.size (); s++) {
		json_t *section = json_array_get (list, s);
		json_t *data = json_object_get (section, "data");
		GameSection &own = sections[s];
		unsigned have;

		if (strcmp (own.name, section_name) || own.chosen < 0 || !own.buffer)
			continue;
		have = region_bit (json_array_get (data, own.chosen));
		if (!have || (have & mask) || !mask)
			return false;

		RomSet *set = open_romset (entry, path.c_str (), archive.c_str ());
		int best = -1;
		const char *best_name = "";

		/* The latest revision there: their letters follow the part number */
		for (size_t i = 0; i < json_array_size (data); i++) {
			json_t *datum = json_array_get (data, i);
			const char *name = json_string_value (json_object_get (datum, "name"));

			if ((region_bit (datum) & mask) && has_datum (datum, set) && strcmp (name, best_name) > 0) {
				best = (int) i;
				best_name = name;
			}
		}
		if (best >= 0) {
			Buffer *buffer = load_datum (json_array_get (data, best), set);

			if (buffer) {
				destroy (own.buffer);
				own.buffer = buffer;
				own.chosen = best;
				changed = true;
			}
		}
		destroy (set);
		break;
	}
	return changed;
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

/* Each word of find, whatever the case, somewhere in the set's name, its
 * parent's, its title or its maker: "f355 twin", "TAXI", "sega 2000" */
bool
GameList::matches (const GameEntry &entry, const char *find)
{
	std::string hay = std::string (entry.name) + " " + entry.parent + " " + entry.description + " " +
	                  entry.manufacturer + " " + entry.year;
	std::string word;
	const char *p = find;

	for (char &c : hay)
		c = (char) tolower ((unsigned char) c);
	for (;;) {
		if (!*p || isspace ((unsigned char) *p)) {
			if (!word.empty () && hay.find (word) == std::string::npos)
				return false;
			word.clear ();
			if (!*p)
				return true;
		} else
			word += (char) tolower ((unsigned char) *p);
		p++;
	}
}

void
GameList::print_found (FILE *fp, const char *find)
{
	unsigned n = 0;

	for (const GameEntry &entry : entries) {
		char title[128], version[32];

		if (!strcmp (entry.name, entry.bios) || !matches (entry, find))
			continue;
		if (!n++)
			fprintf (fp, "%-10s  %-42s  %-9s  %-7s  %s\n", "set", "title", "version", "machine", "year, maker");
		split_description (entry.description, title, sizeof (title), version, sizeof (version));
		fprintf (fp, "%-10s  %-42s  %-9s  %-7s  %s  %s\n", entry.name, title, version,
		         json_string_value (json_object_get (entry.root, "mach")) ?
		         json_string_value (json_object_get (entry.root, "mach")) : "?",
		         entry.year, entry.manufacturer);
	}
	fprintf (fp, "%s%u game%s for \"%s\"\n", n ? "\n" : "", n, n == 1 ? "" : "s", find);
}

void
GameList::print (FILE *fp, const char *path, const char *find)
{
	unsigned i, count = 0;

	for (const GameEntry &entry : entries)
		count += strcmp (entry.name, entry.bios) != 0 && (!find || matches (entry, find));
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
		if (!strcmp (entry->name, entry->bios) || (find && !matches (*entry, find)))
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

		/* The MAME sets: its own, its parent's, the BIOS', each a zip
		 * or a 7z (hl::RomSet) */
		fprintf (fp, "  sets:               %s", entry->name);
		if (entry->parent[0])
			fprintf (fp, ", %s (parent)", entry->parent);
		if (entry->bios[0])
			fprintf (fp, ", %s (BIOS)", entry->bios);
		fprintf (fp, ", each a .zip or a .7z\n");

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
