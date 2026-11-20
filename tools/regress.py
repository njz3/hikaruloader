# Hikaruloader
# Copyright (C) 2026, Benjamin Maurin (njz3)
#
# Released under the GNU General Public License, version 3 or (at your
# option) any later version; see LICENSE.

"""Two builds compared frame by frame (DEVELOPMENT.md, "Regressions"): each
game run from its boot in agent mode, in a fresh write directory, its
memories hashed at the end of each frame (HR_HASH); the first frame where
the two differ, and which memories. With one build (no --b), the same program
twice: whether its runs repeat.

    python tools/regress.py --a build-static/Release --roms C:/roms crzytaxi sstrkfgt
    python tools/regress.py --a old/ --b new/ --roms C:/roms --all --frames 1800 --jobs 6

A program is a folder holding hikaruloader.exe (and its data/), or the exe.
--all takes each set of data/games.json whose zip is in --roms; a set that
does not start (a file missing) is told and left. The runs' files stay in
--work (a temporary folder by default): run_<set>_<a|b>/, hashes.txt and the
program's log. Then HR_HASH_BLOCKS=<frame> on a run finds the 64 KB blocks
that differ (DEVELOPMENT.md)."""

import argparse
import concurrent.futures
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def program(path):
    if os.path.isdir(path):
        path = os.path.join(path, 'hikaruloader.exe' if os.name == 'nt' else 'hikaruloader')
    if not os.path.isfile(path):
        sys.exit('regress: no program at %s' % path)
    return os.path.abspath(path)


def run(exe, game, roms, frames, folder, ini, timeout):
    """One run from boot: (hashes file, None) or (None, why it failed)"""
    shutil.rmtree(folder, ignore_errors=True)
    wd = os.path.join(folder, 'wd')
    os.makedirs(wd)
    with open(os.path.join(wd, 'hikaruloader.ini'), 'w') as f:
        f.write(ini)
    hashes = os.path.join(folder, 'hashes.txt')
    env = dict(os.environ, HR_HASH=hashes, HR_NO_AUDIO='1')
    env.pop('HR_HASH_BLOCKS', None)
    err = open(os.path.join(folder, 'stderr.txt'), 'wb')
    p = subprocess.Popen([exe, '--write-dir', wd, '-R', roms, '-r', game, '-agent'],
                         cwd=os.path.dirname(exe), stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=err, env=env)
    timer = threading.Timer(timeout, p.kill)
    timer.start()
    try:
        line = b''
        while not line.startswith(b'ready'):
            line = p.stdout.readline()
            if not line:
                return None, 'did not start (see %s)' % os.path.join(folder, 'stderr.txt')
        p.stdin.write(b'step %d\n' % frames)
        p.stdin.flush()
        answer = p.stdout.readline().decode(errors='replace').strip()
        try:
            p.stdin.write(b'quit\n')
            p.stdin.flush()
        except OSError:
            pass
        p.wait(30)
        if not answer.startswith('ok'):
            return hashes, 'stopped: %s' % (answer or 'no answer (timed out or crashed)')
        return hashes, None
    finally:
        timer.cancel()
        if p.poll() is None:
            p.kill()
        err.close()


def read(path):
    frames = []
    if path and os.path.exists(path):
        for line in open(path):
            if line.startswith('F '):
                parts = line.split()
                frames.append((int(parts[1]), dict(kv.split('=', 1) for kv in parts[2:])))
    return frames


def compare(a, b):
    """'' when the same, else the first frame and memories that differ"""
    for (fa, ma), (fb, mb) in zip(a, b):
        if ma != mb:
            names = sorted(k for k in set(ma) | set(mb) if ma.get(k) != mb.get(k))
            return 'differ at frame %d: %s' % (fa, ', '.join(names))
    if len(a) != len(b):
        return 'one stopped at frame %d, the other at %d' % (len(a), len(b))
    return ''


def all_sets(roms):
    """Those whose zip, or their parent's (a clone in a merged set), is in one
    of the folders (several separated by ';', as -R takes them)"""
    games = json.load(open(os.path.join(ROOT, 'data', 'games.json'), encoding='utf-8'))
    folders = [f for f in roms.split(';') if f]

    def there(name):
        return any(os.path.exists(os.path.join(f, name + ext)) for f in folders for ext in ('.zip', '.7z'))
    return [g['name'] for g in games['roms'] if there(g['name']) or (g.get('parent') and there(g['parent']))]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--a', required=True, help='the reference program')
    ap.add_argument('--b', help='the program checked (default: --a again)')
    ap.add_argument("--roms", required=True, help="the ROMs folder, several separated by ';' (zips, CHDs)")
    ap.add_argument('--frames', type=int, default=1800)
    ap.add_argument('--jobs', type=int, default=4, help='games run at once (each run of a pair too)')
    ap.add_argument('--all', action='store_true', help="every set of data/games.json found in --roms")
    ap.add_argument('--ini', help="a hikaruloader.ini for the runs (default: an empty one, the defaults)")
    ap.add_argument('--work', help='where the runs go (default: a temporary folder)')
    ap.add_argument('--timeout', type=float, default=1200, help='seconds a run may take')
    ap.add_argument('sets', nargs='*')
    args = ap.parse_args()

    a = program(args.a)
    b = program(args.b) if args.b else a
    sets = args.sets + (all_sets(args.roms) if args.all else [])
    if not sets:
        sys.exit('regress: no set (name them, or --all)')
    ini = open(args.ini).read() if args.ini else '[Display]\n'
    work = args.work or tempfile.mkdtemp(prefix='hlregress_')
    os.makedirs(work, exist_ok=True)
    print('regress: %s against %s, %d sets, %d frames, in %s' %
          (b, a if b != a else 'itself', len(sets), args.frames, work), flush=True)

    def pair(game):
        out = {}

        def one(which, exe):
            out[which] = run(exe, game, args.roms, args.frames,
                             os.path.join(work, 'run_%s_%s' % (game, which)), ini, args.timeout)
        ts = [threading.Thread(target=one, args=w) for w in (('a', a), ('b', b))]
        [t.start() for t in ts]
        [t.join() for t in ts]
        (ha, ea), (hb, eb) = out['a'], out['b']
        if ea and ea.startswith('did not start') and eb and eb.startswith('did not start'):
            return game, 'left: ' + ea
        verdict = compare(read(ha), read(hb))
        problems = '; '.join('%s %s' % (w, e) for w, e in (('a', ea), ('b', eb)) if e)
        return game, (verdict or 'same over %d frames' % len(read(ha))) + ('; ' + problems if problems else '')

    start = time.time()
    counts = {'same': 0, 'differ': 0, 'left': 0}
    with concurrent.futures.ThreadPoolExecutor(max(1, args.jobs // 2)) as pool:
        for game, verdict in pool.map(pair, sets):
            key = 'same' if verdict.startswith('same') and ';' not in verdict else 'left' if verdict.startswith('left') else 'differ'
            counts[key] += 1
            print('%-12s %s' % (game, verdict), flush=True)
    print('regress: %d the same, %d different, %d left, in %.0f s' %
          (counts['same'], counts['differ'], counts['left'], time.time() - start))
    return 1 if counts['differ'] else 0


if __name__ == '__main__':
    sys.exit(main())
