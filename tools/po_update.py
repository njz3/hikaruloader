#!/usr/bin/env python3
#
# Hikaruloader (formerly Valkyrie)
# Copyright (C) 2026, Benjamin Maurin (njz3)
#
# Hikaruloader is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Hikaruloader is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with Hikaruloader.  If not, see <http://www.gnu.org/licenses/>.

"""The interface's translations: data/lang/hikaruloader.pot, the template,
made again from the texts the sources give to tr () or mark with N_ ()
(a table's texts, given to tr () later); each data/lang/*.po
brought up to date with it: its translations kept, the new texts added
untranslated, those gone kept as obsolete (#~) and taken again when they
come back. As xgettext --keyword=tr --keyword=N_
and msgmerge would, without gettext.

    po_update.py                 the template and every catalog
    po_update.py --check         lists, per catalog, the texts left to translate
    po_update.py --new CODE NAME a catalog for a new language ("de" "Deutsch")
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANG = os.path.join(ROOT, "data", "lang")
POT = os.path.join(LANG, "hikaruloader.pot")
LITERAL = r'"(?:[^"\\]|\\.)*"'
ESCAPES = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\"}


def unescape(s):
    return re.sub(r"\\(.)", lambda m: ESCAPES.get(m.group(1), "\\" + m.group(1)), s)


def escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t")


def extract():
    """The texts given to tr () or N_ (), adjacent literals joined, with the files
    that have them, in the order first met"""
    texts = {}
    for folder, _, files in sorted(os.walk(os.path.join(ROOT, "src"))):
        for name in sorted(files):
            if not name.endswith((".cpp", ".h")):
                continue
            path = os.path.join(folder, name)
            source = open(path, encoding="utf-8").read()
            rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
            for m in re.finditer(r"\b(?:tr|N_)\s*\(\s*((?:" + LITERAL + r"\s*)+)\)", source):
                text = "".join(unescape(lit[1:-1]) for lit in re.findall(LITERAL, m.group(1)))
                texts.setdefault(text, [])
                if rel not in texts[text]:
                    texts[text].append(rel)
    return texts


def parse(path):
    """A catalog: its header and its entries, {msgid: (msgstr, fuzzy)}, in
    order; the obsolete ones apart"""
    header, entries, obsolete = "", {}, {}
    msgid = msgstr = None
    current = None
    fuzzy = next_fuzzy = False
    is_obsolete = False

    def finish():
        nonlocal msgid, msgstr, current, fuzzy, is_obsolete, header
        if msgid is not None:
            if msgid == "":
                header = msgstr or ""
            elif is_obsolete:
                obsolete[msgid] = (msgstr or "", fuzzy)
            else:
                entries[msgid] = (msgstr or "", fuzzy)
        msgid = msgstr = current = None
        fuzzy = is_obsolete = False

    for line in open(path, encoding="utf-8-sig"):
        line = line.strip()
        old = line.startswith("#~")
        if old:
            line = line[2:].strip()
        if not line:
            continue
        if line.startswith("#"):
            if current == "msgstr":
                finish()
            if line.startswith("#,") and "fuzzy" in line:
                next_fuzzy = True
            continue
        if line.startswith('"'):
            value = unescape(line[1:-1])
            if current == "msgid":
                msgid += value
            elif current == "msgstr":
                msgstr += value
            continue
        keyword, _, rest = line.partition(" ")
        value = unescape(rest.strip()[1:-1])
        if keyword == "msgid":
            if current == "msgstr":
                finish()
            msgid, current = value, "msgid"
            fuzzy, next_fuzzy, is_obsolete = next_fuzzy, False, old
        elif keyword in ("msgstr", "msgstr[0]"):
            msgstr, current = value, "msgstr"
        else:
            current = None	# msgctxt, plurals: not used by the interface
    finish()
    return header, entries, obsolete


def write(path, header, texts, entries, obsolete):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write('msgid ""\nmsgstr ""\n')
        for line in header.splitlines():
            f.write('"%s\\n"\n' % escape(line))
        for text, files in texts.items():
            msgstr, fuzzy = entries.get(text) or obsolete.get(text) or ("", False)
            f.write("\n#: %s\n" % " ".join(files))
            if fuzzy:
                f.write("#, fuzzy\n")
            f.write('msgid "%s"\nmsgstr "%s"\n' % (escape(text), escape(msgstr)))
        for text, (msgstr, fuzzy) in obsolete.items():
            if text in texts:
                continue
            f.write('\n#~ msgid "%s"\n#~ msgstr "%s"\n' % (escape(text), escape(msgstr)))


def header_for(code, name):
    return "\n".join([
        "Project-Id-Version: Hikaru Loader",
        "Language: %s" % code,
        "X-Language-Name: %s" % name,
        "MIME-Version: 1.0",
        "Content-Type: text/plain; charset=UTF-8",
        "Content-Transfer-Encoding: 8bit",
    ])


def catalogs():
    return sorted(os.path.join(LANG, f) for f in os.listdir(LANG) if f.endswith(".po"))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="list the texts left to translate")
    parser.add_argument("--new", nargs=2, metavar=("CODE", "NAME"), help="a catalog for a new language")
    args = parser.parse_args()

    texts = extract()
    if args.check:
        left = 0
        for path in catalogs():
            _, entries, _ = parse(path)
            todo = [t for t in texts if not entries.get(t, ("", False))[0] or entries[t][1]]
            left += len(todo)
            print("%s: %d of %d translated" % (os.path.basename(path), len(texts) - len(todo), len(texts)))
            for t in todo:
                print("    %r" % t)
        return 1 if left else 0

    os.makedirs(LANG, exist_ok=True)
    write(POT, header_for("", ""), texts, {}, {})
    print("%s: %d texts" % (os.path.relpath(POT, ROOT), len(texts)))
    if args.new:
        code, name = args.new
        path = os.path.join(LANG, code + ".po")
        if os.path.exists(path):
            sys.exit("%s exists already" % path)
        write(path, header_for(code, name), texts, {}, {})
    for path in catalogs():
        header, entries, obsolete = parse(path)
        obsolete.update({k: v for k, v in entries.items() if k not in texts})
        write(path, header, texts, entries, obsolete)
        done = sum(1 for t in texts if entries.get(t, ("", False))[0] and not entries[t][1])
        print("%s: %d of %d translated" % (os.path.relpath(path, ROOT), done, len(texts)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
