#!/usr/bin/env python3
"""Rootfs manifest for the web build (M4, W1).

Usage: mkmanifest.py [--product] ROOTFS OUT_JSON

Walks ROOTFS (a guest root directory) and writes one entry per path to OUT_JSON:

  {"version": 1, "root": "build/rootfs", "count": N,
   "entries": [{"path": "/system/bin", "type": "dir", "mode": 493},
               {"path": "/system/bin/toybox", "type": "file", "mode": 493, "size": 123},
               {"path": "/system/bin/linker64", "type": "symlink", "mode": 511,
                "target": "/apex/com.android.runtime/bin/linker64"}]}

Entries are sorted by path, so every directory comes before its children. The worker
creates them in that order. Regular files are not copied: the web server serves them
from ROOTFS, and the worker creates lazy MEMFS files that fetch them on first use.

Default (the builder image, build/rootfs): the guest's /data/hello.dex must be present
(make build/rootfs/data/hello.dex).

--product (the product tree, build/rootfs-xiaoji, for the "Boot Android" page): the same walk, with
these differences. The writable trees (/data, /cache, /metadata) keep their directories, and their
contents are left out: the browser boot starts from MEMFS, so nothing the host copy holds there is
used. The metadata sidecar (.xiaoji-meta, and its temporary file) is left out, because the guest
cannot see it either. The required entries are the product's init and linker config.

/proc, /sys and /dev are synthesized by the engine, so their contents are left out. The
directories themselves stay, as empty entries: the engine resolves guest paths one
component at a time against the tree, so /proc/self/exe needs /proc to exist.
"""
import json
import os
import stat
import sys

SKIP_TOP = {"proc", "sys", "dev"}
# Writable trees: the directory stays, the contents are not listed (product mode only).
WRITABLE_TOP = {"data", "cache", "metadata"}
SIDECAR_NAMES = {".xiaoji-meta", ".xiaoji-meta.tmp"}
REQUIRED = ["/data/hello.dex", "/linkerconfig", "/apex/apex-info-list.xml"]
REQUIRED_PRODUCT = ["/system/bin/init", "/linkerconfig", "/apex/apex-info-list.xml",
                    "/system/framework/arm64/boot.art"]


def walk(root, product=False):
    entries = []
    skipped = []
    stack = [("", root)]
    while stack:
        rel, host = stack.pop()
        with os.scandir(host) as it:
            children = sorted(it, key=lambda e: e.name)
        for de in children:
            guest = rel + "/" + de.name
            st = os.lstat(de.path)
            mode = stat.S_IMODE(st.st_mode)
            if rel == "" and de.name in SKIP_TOP:
                if stat.S_ISDIR(st.st_mode):
                    entries.append({"path": guest, "type": "dir", "mode": mode})
                continue
            if product and rel == "" and de.name in SIDECAR_NAMES:
                continue
            if product and rel == "" and de.name in WRITABLE_TOP and stat.S_ISDIR(st.st_mode):
                entries.append({"path": guest, "type": "dir", "mode": mode})
                continue
            if stat.S_ISLNK(st.st_mode):
                entries.append({"path": guest, "type": "symlink", "mode": mode,
                                "target": os.readlink(de.path)})
            elif stat.S_ISDIR(st.st_mode):
                entries.append({"path": guest, "type": "dir", "mode": mode})
                stack.append((guest, de.path))
            elif stat.S_ISREG(st.st_mode):
                entries.append({"path": guest, "type": "file", "mode": mode,
                                "size": st.st_size})
            else:
                skipped.append(guest)
    entries.sort(key=lambda e: e["path"])
    return entries, skipped


def main(argv):
    args = argv[1:]
    product = False
    if args and args[0] == "--product":
        product = True
        args = args[1:]
    if len(args) != 2:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    root, out = args
    entries, skipped = walk(root, product)
    have = {e["path"] for e in entries}
    required = REQUIRED_PRODUCT if product else REQUIRED
    missing = [p for p in required if p not in have]
    if missing:
        print("mkmanifest: missing from rootfs: " + ", ".join(missing), file=sys.stderr)
        return 1
    if skipped:
        print("mkmanifest: skipped %d non-regular entries (first: %s)"
              % (len(skipped), skipped[0]), file=sys.stderr)
    doc = {"version": 1, "root": root, "count": len(entries), "entries": entries}
    tmp = out + ".tmp"
    with open(tmp, "w") as f:
        json.dump(doc, f, separators=(",", ":"))
        f.write("\n")
    os.replace(tmp, out)
    nfile = sum(1 for e in entries if e["type"] == "file")
    total = sum(e.get("size", 0) for e in entries)
    print("mkmanifest: %d entries (%d files, %d bytes) -> %s" % (len(entries), nfile, total, out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
