#!/usr/bin/env python3
"""Packs an engine snapshot for the page (docs/snapshot.md, S4). Stdlib only.

Usage: snappack.py SNAPSHOT OUT_DIR --tree MANIFEST [--chunk-size 262144] [--pack-max 16777216]

SNAPSHOT is a file written by the web engine's --snapshot-out (engine/snap.c). OUT_DIR is the chunk store
(build/web/xj). The image is cut into chunks (zlib, level 9; a chunk's id is the first 32 hex digits of the SHA-256 of
its raw bytes, as tools/pack.py names them, so equal chunks are stored once), which go only into packs/<id>.bin (in
first-use order, each its own zlib stream, at most --pack-max bytes a pack): the page restores eagerly, from the packs,
and no chunk file is written (the deployment's file count stays the tree's). The packs of the snapshot.json this
replaces are removed, unless the tree's manifest uses them. The manifest goes to snapshot.json:

  version, format "xiaoji-snapshot", build    the engine build that wrote it (the page restores only into that build)
  tree                                        the SHA-256 of the tree manifest the snapshot's engine booted (--tree)
  head                                        the file's first bytes (the 4 KiB header and the writable trees):
                                              {bytes, chunks}
  ranges, extents                             the image: the ranges it covers, and each run of non-zero memory as
                                              [address, length, [chunk ids]]
  packs                                       [{file, bytes, chunks: [[id, offset, zlib bytes, raw bytes], ...]}]
  image_bytes, download_bytes                 memory written, and the bytes of the packs
"""
import argparse
import hashlib
import json
import os
import struct
import sys
import zlib

ID_HEX = 32
MAGIC = b"XJSNAP\0\1"


def header(f):
    h = f.read(4096)
    if h[:8] != MAGIC:
        raise SystemExit("snappack: not a snapshot (magic)")
    version, flags = struct.unpack_from("<II", h, 8)
    build = h[16:80].split(b"\0", 1)[0].decode()
    mono, real, icount, nranges = struct.unpack_from("<QQQQ", h, 80)
    ranges = [list(struct.unpack_from("<QQ", h, 112 + 16 * i)) for i in range(nranges)]
    nextents, ext_off, data_off, image_bytes = struct.unpack_from("<QQQQ", h, 240)
    return {"version": version, "flags": flags, "build": build, "icount": icount, "ranges": ranges,
            "nextents": nextents, "ext_off": ext_off, "data_off": data_off, "image_bytes": image_bytes}


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("snapshot")
    ap.add_argument("out")
    ap.add_argument("--tree", required=True, help="the tree manifest the snapshot's engine booted")
    ap.add_argument("--chunk-size", type=int, default=262144)
    ap.add_argument("--pack-max", type=int, default=16 * 1024 * 1024)
    a = ap.parse_args(argv)
    os.makedirs(os.path.join(a.out, "packs"), exist_ok=True)
    old_packs = set()
    try:
        with open(os.path.join(a.out, "snapshot.json")) as o:
            old = json.load(o)
            old_packs = {p["file"] for p in old["packs"]}
            old_packs |= {p["file"] for p in old.get("prefetch", {}).get("packs", [])}  # tools/prefetch.py's
        with open(a.tree) as o:
            old_packs -= {p["file"] for p in json.load(o).get("packs", [])}
    except (OSError, ValueError, KeyError):
        pass
    with open(a.tree, "rb") as t:
        tree = hashlib.sha256(t.read()).hexdigest()

    seen = {}  # id -> (zlib bytes, raw length)
    order = []  # ids in first-use order

    def chunk(raw):
        cid = hashlib.sha256(raw).hexdigest()[:ID_HEX]
        if cid not in seen:
            seen[cid] = (zlib.compress(raw, 9), len(raw))
            order.append(cid)
        return cid

    with open(a.snapshot, "rb") as f:
        h = header(f)
        f.seek(0)
        head = f.read(h["ext_off"])
        head_ids = [chunk(head[i:i + a.chunk_size]) for i in range(0, len(head), a.chunk_size)]
        f.seek(h["ext_off"])
        table = f.read(16 * h["nextents"])
        extents = []
        f.seek(h["data_off"])
        for k in range(h["nextents"]):
            addr, length = struct.unpack_from("<QQ", table, 16 * k)
            ids = []
            left = length
            while left:
                n = min(a.chunk_size, left)
                ids.append(chunk(f.read(n)))
                left -= n
            extents.append([addr, length, ids])

    packs, cur, cur_bytes = [], [], 0

    def flush():
        nonlocal cur, cur_bytes
        if not cur:
            return
        blob = b"".join(seen[c][0] for c in cur)
        pid = hashlib.sha256(blob).hexdigest()[:ID_HEX]
        name = "packs/%s.bin" % pid
        with open(os.path.join(a.out, name), "wb") as o:
            o.write(blob)
        at, rows = 0, []
        for c in cur:
            z, raw = seen[c]
            rows.append([c, at, len(z), raw])
            at += len(z)
        packs.append({"file": name, "bytes": len(blob), "chunks": rows})
        cur, cur_bytes = [], 0

    for c in order:
        z = seen[c][0]
        if cur and cur_bytes + len(z) > a.pack_max:
            flush()
        cur.append(c)
        cur_bytes += len(z)
    flush()

    m = {"version": 1, "format": "xiaoji-snapshot", "build": h["build"], "tree": tree, "chunk_size": a.chunk_size,
         "id_hex": ID_HEX, "icount": h["icount"], "head": {"bytes": len(head), "chunks": head_ids},
         "ranges": h["ranges"], "extents": extents, "packs": packs, "image_bytes": h["image_bytes"],
         "download_bytes": sum(p["bytes"] for p in packs)}
    tmp = os.path.join(a.out, "snapshot.json.tmp")
    with open(tmp, "w") as o:
        json.dump(m, o, separators=(",", ":"))
    os.replace(tmp, os.path.join(a.out, "snapshot.json"))
    for f in old_packs - {p["file"] for p in packs}:
        try:
            os.remove(os.path.join(a.out, f))
        except OSError:
            pass
    print("snappack: %s: %d MiB of memory, %d extents, %d distinct chunks, %d packs, %.1f MiB to download" % (
        h["build"], h["image_bytes"] >> 20, len(extents), len(order), len(packs),
        m["download_bytes"] / 1048576.0))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
