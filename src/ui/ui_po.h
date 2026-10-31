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

// A gettext catalog (.po): the interface's translations, read without gettext.
//
// The parts of the format the interface uses: msgid and msgstr, each one or
// more quoted strings joined, with the C escapes (\n \t \" \\); the comments;
// a "#, fuzzy" entry left out, as gettext does; an empty msgstr left out (the
// English text then); msgctxt and the plural forms read, the plurals' first
// form kept. The header (the msgid "" entry) gives its fields: Language,
// X-Language-Name (the language's own name, for the settings' list).

#ifndef __HL_UI_PO_H__
#define __HL_UI_PO_H__

#include <string>
#include <unordered_map>

namespace ui
{

class PoCatalog
{
public:
    // false when the file cannot be read; what was parsed up to an error is kept
    bool load(const char *path);
    bool parse(const std::string &text);

    // The translation of an English text, nullptr when there is none
    const std::string *find(const std::string &msgid) const;
    // A header field ("Language"), empty when absent
    std::string header(const char *field) const;
    size_t size() const { return texts.size(); }

private:
    std::unordered_map<std::string, std::string> texts;
    std::string headerText;
};

} // namespace ui

#endif // __HL_UI_PO_H__
