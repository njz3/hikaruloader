# Hikaruloader (formerly Valkyrie)
# Copyright (C) 2026, Benjamin Maurin (njz3)
#
# Released under the GNU General Public License, version 3 or (at your
# option) any later version; see LICENSE.

"""Makes a release archive from a static build (INSTALL.md, chapter 2,
"Making a release archive"; DEVELOPMENT.md, "Versions and releases"):
dist/hikaruloader-<version>-win64.zip, its folder hikaruloader-<version>/
holding the program, its data/ and the documents, and its SHA-256 beside it.

It checks first that the build is a release (a tag vX.Y.Z on a clean commit,
or HL_FORCE_VERSION for a trial), that the program carries that version, and
that it needs no DLL but Windows' own (a static build).

Usage: python tools/make_release.py [--build build-static] [--config Release]
                                     [--out dist] [--allow-dev]"""

import argparse
import hashlib
import os
import re
import struct
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The documents for those who play (TODO.md stays in the sources)
DOCS = ["README.md", "INSTALL.md", "CHANGELOG.md", "LICENSE"]

# Windows' own DLLs, which every Windows has; anything else is not static
SYSTEM_DLLS = {
    "kernel32.dll", "user32.dll", "gdi32.dll", "opengl32.dll", "ws2_32.dll",
    "winmm.dll", "imm32.dll", "ole32.dll", "oleaut32.dll", "version.dll",
    "advapi32.dll", "setupapi.dll", "shell32.dll", "cfgmgr32.dll",
    "dinput8.dll", "xinput1_4.dll", "hid.dll", "dwmapi.dll", "uxtheme.dll",
    "comdlg32.dll", "shlwapi.dll", "dbghelp.dll", "bcrypt.dll",
}


def fail(msg):
    sys.exit("make_release: " + msg)


def read_version(build):
    """HL_VERSION and HL_VERSION_DEV of the build's hl_version.h"""
    path = os.path.join(build, "generated", "hl_version.h")
    if not os.path.exists(path):
        fail("%s missing: configure and build first" % path)
    text = open(path, encoding="utf-8").read()
    version = re.search(r'#define HL_VERSION "([^"]+)"', text)
    dev = re.search(r"#define HL_VERSION_DEV (\d)", text)
    if not version or not dev:
        fail("%s: no HL_VERSION" % path)
    return version.group(1), dev.group(1) == "1"


def pe_imports(path):
    """The DLLs a PE32+ program imports (its import directory)"""
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        fail("%s is not a Windows program" % path)
    sections, opt_size = struct.unpack_from("<H12xH", data, pe + 6)
    opt = pe + 24
    if struct.unpack_from("<H", data, opt)[0] != 0x20B:
        fail("%s is not a 64-bit program" % path)
    import_rva = struct.unpack_from("<I", data, opt + 112 + 8)[0]
    table = []
    for i in range(sections):
        base = opt + opt_size + 40 * i
        vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, base + 8)
        table.append((vaddr, max(vsize, rsize), raddr))

    def offset(rva):
        for vaddr, size, raddr in table:
            if vaddr <= rva < vaddr + size:
                return rva - vaddr + raddr
        fail("%s: an address out of its sections" % path)

    names = []
    at = offset(import_rva)
    while True:
        name_rva = struct.unpack_from("<I", data, at + 12)[0]
        if not name_rva:
            break
        start = offset(name_rva)
        names.append(data[start:data.index(b"\0", start)].decode("ascii"))
        at += 20
    return names


def pe_version(path):
    """The ProductVersion string of the program's version resource"""
    data = open(path, "rb").read()
    key = "ProductVersion".encode("utf-16-le") + b"\0\0"
    at = data.find(key)
    if at < 0:
        return None
    at += len(key)
    at += (-at) % 4  # the value, 32-bit aligned
    end = at
    while data[end:end + 2] != b"\0\0":
        end += 2
    return data[at:end].decode("utf-16-le")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--build", default=os.path.join(ROOT, "build-static"))
    parser.add_argument("--config", default="Release")
    parser.add_argument("--out", default=os.path.join(ROOT, "dist"))
    parser.add_argument("--allow-dev", action="store_true",
                        help="package a development snapshot too")
    args = parser.parse_args()

    version, dev = read_version(args.build)
    if dev and not args.allow_dev:
        fail("%s is a development snapshot: tag the commit vX.Y.Z, or configure "
             "with -DHL_FORCE_VERSION=X.Y.Z for a trial release" % version)

    bindir = os.path.join(args.build, args.config)
    exe = os.path.join(bindir, "hikaruloader.exe")
    if not os.path.exists(exe):
        fail("%s missing: build it first" % exe)
    exe_version = pe_version(exe)
    if exe_version != version:
        fail("%s says %s, the build %s: build it again" % (exe, exe_version, version))
    dlls = pe_imports(exe)
    others = [d for d in dlls if d.lower() not in SYSTEM_DLLS]
    if others:
        fail("%s needs %s: not a static build (INSTALL.md)" % (exe, ", ".join(others)))
    for doc in DOCS:
        if not os.path.exists(os.path.join(ROOT, doc)):
            fail("%s missing" % doc)

    name = "hikaruloader-%s" % version
    os.makedirs(args.out, exist_ok=True)
    archive = os.path.join(args.out, name + "-win64.zip")
    files = [(exe, "hikaruloader.exe")]
    data = os.path.join(bindir, "data")
    for folder, _, names in os.walk(data):
        for n in sorted(names):
            path = os.path.join(folder, n)
            files.append((path, os.path.relpath(path, bindir).replace(os.sep, "/")))
    files += [(os.path.join(ROOT, doc), doc) for doc in DOCS]
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for path, arcname in files:
            z.write(path, name + "/" + arcname)

    digest = hashlib.sha256(open(archive, "rb").read()).hexdigest()
    with open(archive + ".sha256", "w", encoding="ascii", newline="\n") as f:
        f.write("%s  %s\n" % (digest, os.path.basename(archive)))

    print("%s: %d files, %.1f MB" % (archive, len(files), os.path.getsize(archive) / 1e6))
    print("  version %s%s" % (version, " (development snapshot)" if dev else ""))
    print("  needs %s" % ", ".join(dlls))
    print("  sha256 %s" % digest)


if __name__ == "__main__":
    main()
