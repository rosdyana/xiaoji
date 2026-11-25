#!/usr/bin/env python3
"""Copies the chunk store SRC (build/web/xj) to DST with only the files its manifests name.

Every *.json at the top of SRC is copied. Any string in them that names a file under packs/ is kept,
and any 32-hex-digit chunk id is kept as chunks/<id>.z when that file exists. Old snapshots and trees
leave packs nothing refers to; they are left out.
"""
import json
import os
import re
import shutil
import sys

HEX = re.compile(r'^[0-9a-f]{32}$')


def walk(v, out):
    if isinstance(v, str):
        out.add(v)
    elif isinstance(v, list):
        for x in v:
            walk(x, out)
    elif isinstance(v, dict):
        for x in v.values():
            walk(x, out)


def main(src, dst):
    os.makedirs(dst, exist_ok=True)
    names = set()
    for f in sorted(os.listdir(src)):
        if f.endswith('.json') and os.path.isfile(os.path.join(src, f)):
            shutil.copy2(os.path.join(src, f), os.path.join(dst, f))
            with open(os.path.join(src, f)) as fh:
                walk(json.load(fh), names)
    keep = set()
    for s in names:
        if s.startswith('packs/') and '..' not in s:
            keep.add(s)
        elif HEX.match(s):
            keep.add('chunks/' + s + '.z')
    copied = size = 0
    for rel in sorted(keep):
        p = os.path.join(src, rel)
        if not os.path.isfile(p):
            continue
        os.makedirs(os.path.dirname(os.path.join(dst, rel)), exist_ok=True)
        shutil.copy2(p, os.path.join(dst, rel))
        copied += 1
        size += os.path.getsize(p)
    total = sum(len(fs) for _, _, fs in os.walk(src))
    print(f'site-prune: kept {copied} of {total} files ({size / 2**20:.1f} MiB)')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
