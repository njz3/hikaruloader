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

// The gettext catalogs' reader (src/ui/ui_po.cpp): the format's corners, then
// the French catalog given as argument (data/lang/fr.po).

#include <cstdio>
#include <string>

#include "ui/ui_po.h"

static int failures = 0;

static void check(bool ok, const char *what)
{
    if (!ok)
    {
        printf("FAILED: %s\n", what);
        failures++;
    }
}

static bool translates(const ui::PoCatalog &po, const char *msgid, const char *msgstr)
{
    const std::string *found = po.find(msgid);
    return found && *found == msgstr;
}

int main(int argc, char **argv)
{
    ui::PoCatalog po;
    bool ok = po.parse(
        "# A translator's comment\n"
        "msgid \"\"\n"
        "msgstr \"\"\n"
        "\"Language: xx\\n\"\n"
        "\"X-Language-Name: Test\\n\"\n"
        "\n"
        "#: src/ui/ui_common.cpp\n"
        "msgid \"Settings\"\n"
        "msgstr \"Réglages\"\n"
        "\n"
        "msgid \"\"\n"
        "\"Joined \"\n"
        "\"strings\"\n"
        "msgstr \"Chaînes \"\n"
        "\"jointes\"\n"
        "\n"
        "msgid \"Escapes: \\\"\\t\\\\\\n\"\n"
        "msgstr \"Échappements : \\\"\\t\\\\\\n\"\n"
        "\n"
        "#, fuzzy\n"
        "msgid \"Fuzzy\"\n"
        "msgstr \"Approximatif\"\n"
        "\n"
        "msgid \"After the fuzzy one\"\n"
        "msgstr \"Après l'approximatif\"\n"
        "\n"
        "msgid \"Untranslated\"\n"
        "msgstr \"\"\n"
        "\n"
        "msgctxt \"menu\"\n"
        "msgid \"Open\"\n"
        "msgstr \"Ouvrir\"\n"
        "\n"
        "msgid \"%d file\"\n"
        "msgid_plural \"%d files\"\n"
        "msgstr[0] \"%d fichier\"\n"
        "msgstr[1] \"%d fichiers\"\n"
        "\n"
        "#~ msgid \"Gone\"\n"
        "#~ msgstr \"Parti\"\n");

    check(ok, "the sample parses");
    check(po.header("Language") == "xx", "the header's Language");
    check(po.header("X-Language-Name") == "Test", "the header's X-Language-Name");
    check(translates(po, "Settings", "Réglages"), "a plain entry");
    check(translates(po, "Joined strings", "Chaînes jointes"), "strings joined");
    check(translates(po, "Escapes: \"\t\\\n", "Échappements : \"\t\\\n"), "the escapes");
    check(!po.find("Fuzzy"), "a fuzzy entry left out");
    check(translates(po, "After the fuzzy one", "Après l'approximatif"), "the flag not carried over");
    check(!po.find("Untranslated"), "an empty msgstr left out");
    check(translates(po, "Open", "Ouvrir"), "an entry with a context");
    check(translates(po, "%d file", "%d fichier"), "a plural's first form");
    check(!po.find("Gone"), "an obsolete entry left out");

    if (argc > 1)
    {
        ui::PoCatalog fr;
        check(fr.load(argv[1]), "fr.po loads");
        check(fr.header("X-Language-Name") == "Français", "fr.po names its language");
        check(translates(fr, "Settings", "Paramètres"), "fr.po translates Settings");
        check(fr.size() > 100, "fr.po has its texts");
        printf("fr.po: %u texts\n", (unsigned) fr.size());
    }

    printf("%s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
