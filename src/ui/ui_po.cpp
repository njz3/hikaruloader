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

// A gettext catalog (.po); see ui_po.h.

#include "ui/ui_po.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace ui
{
namespace
{

// The quoted string at s (its opening quote), the escapes undone; s past it
bool quoted(const char *&s, std::string &out)
{
    if (*s != '"')
        return false;
    for (s++; *s && *s != '"'; s++)
    {
        if (*s != '\\')
        {
            out += *s;
            continue;
        }
        switch (*++s)
        {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '\0': return false;
        default: out += '\\'; out += *s; break;
        }
    }
    if (*s != '"')
        return false;
    s++;
    return true;
}

const char *skipSpaces(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

} // namespace

bool PoCatalog::load(const char *path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    std::stringstream text;
    text << file.rdbuf();
    return parse(text.str());
}

bool PoCatalog::parse(const std::string &text)
{
    // One entry at a time: its keywords' strings gathered, the entry kept
    // when the next begins (a comment, msgctxt, msgid) or at the end. The
    // flags ("#, fuzzy") are the next entry's
    std::string msgid, msgstr, context, plural, other, *current = nullptr;
    bool fuzzy = false, nextFuzzy = false, inEntry = false, ok = true;

    auto finish = [&]() {
        if (inEntry && !fuzzy)
        {
            if (msgid.empty())
                headerText = msgstr;
            else if (!msgstr.empty())
                texts[msgid] = msgstr;
        }
        msgid.clear();
        msgstr.clear();
        current = nullptr;
        fuzzy = false;
        inEntry = false;
    };
    // An entry's strings are all there once its msgstr is under way
    auto complete = [&]() { return inEntry && (current == &msgstr || current == &other); };

    std::istringstream lines(text);
    std::string line;
    bool first = true;
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        // A UTF-8 byte order mark, as some editors write
        if (first && line.compare(0, 3, "\xEF\xBB\xBF") == 0)
            line.erase(0, 3);
        first = false;

        const char *s = skipSpaces(line.c_str());
        if (*s == '\0')
            continue;
        if (*s == '#')
        {
            // A comment ends an entry; "#, fuzzy, c-format": a flag of the
            // entry to come
            if (complete())
                finish();
            if (s[1] == ',' && strstr(s, "fuzzy"))
                nextFuzzy = true;
            continue;
        }
        if (*s == '"')
        {
            if (!current || !quoted(s, *current))
                ok = false;
            continue;
        }

        // A keyword: msgctxt, msgid, msgid_plural, msgstr, msgstr[n]
        const char *word = s;
        while (*s && *s != ' ' && *s != '\t' && *s != '"')
            s++;
        std::string keyword(word, s - word);
        s = skipSpaces(s);
        if (keyword == "msgctxt" || keyword == "msgid")
        {
            // A new entry, unless its msgctxt opened it
            if (complete())
                finish();
            if (!inEntry)
            {
                inEntry = true;
                fuzzy = nextFuzzy;
                nextFuzzy = false;
            }
            if (keyword == "msgctxt")
            {
                context.clear();
                current = &context;
            }
            else
                current = &msgid;
        }
        else if (keyword == "msgid_plural")
        {
            plural.clear();
            current = &plural;
        }
        else if (keyword == "msgstr" || keyword == "msgstr[0]")
            current = &msgstr;
        else if (keyword.compare(0, 7, "msgstr[") == 0)
        {
            other.clear();
            current = &other;
        }
        else
        {
            ok = false;
            current = nullptr;
            continue;
        }
        if (!quoted(s, *current))
            ok = false;
    }
    finish();
    return ok;
}

const std::string *PoCatalog::find(const std::string &msgid) const
{
    auto it = texts.find(msgid);
    return it != texts.end() ? &it->second : nullptr;
}

std::string PoCatalog::header(const char *field) const
{
    // Lines "Field: value\n"
    const std::string key = std::string(field) + ":";
    size_t at = 0;
    while (at < headerText.size())
    {
        size_t end = headerText.find('\n', at);
        if (end == std::string::npos)
            end = headerText.size();
        if (headerText.compare(at, key.size(), key) == 0)
        {
            size_t value = at + key.size();
            while (value < end && headerText[value] == ' ')
                value++;
            return headerText.substr(value, end - value);
        }
        at = end + 1;
    }
    return std::string();
}

} // namespace ui
