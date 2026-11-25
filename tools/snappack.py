"""Packs an engine snapshot for the page (docs/snapshot.md, S4 and S5). Stdlib only.

Usage: snappack.py SNAPSHOT OUT_DIR --tree MANIFEST [--chunk-size 262144] [--pack-max 4194304]
                   [--eager] [--order TOUCH.json] [--hot-ms MS] [--lazy-pages 32] [--lazy-pack-max 1048576]

SNAPSHOT is a file written by the web engine's --snapshot-out (engine/snap.c). OUT_DIR is the chunk store
(build/web/xj). The image is cut into chunks (zlib, level 9; a chunk's id is the first 32 hex digits of the SHA-256 of
its raw bytes, as tools/pack.py names them, so equal chunks are stored once), which go only into packs/<id>.bin (each
chunk its own zlib stream): no chunk file is written, so the deployment's file count stays the tree's. The packs of the
snapshot.json this replaces are removed, unless the tree's manifest uses them.

Lazy pages (S5; a version 2 snapshot, whose region table says where the page pool's slabs are): the pages of the pool
(guest and memfd pages) are split off the image unless they are hot. Hot means: in --order (a touch order recorded by
tools/snapshot-web.mjs record) at or before --hot-ms after the restore. Everything else of the image (the engine's own
state) is always hot. The page downloads the hot part before main and the lazy pages after, in the background and on
first touch: the touched ones in the recorded first-touch order, then the rest in address order. --eager (or a
version 1 snapshot) keeps the whole image hot, as before S5.

The manifest goes to snapshot.json:

  version, format "xiaoji-snapshot", build    the engine build that wrote it (the page restores only into that build)
  tree                                        the SHA-256 of the tree manifest the snapshot's engine booted (--tree)
  head                                        the file's first bytes (the 4 KiB header and the writable trees):
                                              {bytes, chunks}
  ranges, extents                             the image: the ranges it covers, and each run of hot non-zero memory as
                                              [address, length, [chunk ids]]
  packs                                       the hot chunks: [{file, bytes, chunks: [[id, offset, zlib bytes, raw
                                              bytes], ...]}]
  lazy (version 2 manifests)                  {pages, page_bytes, chunk_pages, map: {file, bytes}, touched, packs,
                                              bytes, raw_bytes}: map is a zlib stream of the lazy pages' numbers
                                              (address / 4096, uint32 little-endian) in their order; lazy chunk k holds
                                              pages [k * chunk_pages, (k + 1) * chunk_pages) of it, their bytes one after
                                              another; touched is the number of pages of the order that were touched in
                                              the recording; packs as above, in order
  image_bytes, download_bytes                 memory written, and the bytes of the hot packs (what comes before main)
  classes                                     raw and zlib bytes by kind of memory (the census of S5)
"""
import argparse
import bisect
import hashlib
import json
import os
import struct
import sys
import zlib

ID_HEX = 32
MAGIC = b"XJSNAP\0\1"
PAGE = 4096
REG_POOL, REG_GL_BLOB = 1, 2


def header(f):
    h = f.read(4096)
    if h[:8] != MAGIC:
        raise SystemExit("snappack: not a snapshot (magic)")
    version, flags = struct.unpack_from("<II", h, 8)
    build = h[16:80].split(b"\0", 1)[0].decode()
    mono, real, icount, nranges = struct.unpack_from("<QQQQ", h, 80)
    ranges = [list(struct.unpack_from("<QQ", h, 112 + 16 * i)) for i in range(nranges)]
    nextents, ext_off, data_off, image_bytes = struct.unpack_from("<QQQQ", h, 240)
    nregions, reg_off = struct.unpack_from("<QQ", h, 1384) if version >= 2 else (0, 0)
    return {"version": version, "flags": flags, "build": build, "icount": icount, "ranges": ranges,
            "nextents": nextents, "ext_off": ext_off, "data_off": data_off, "image_bytes": image_bytes,
            "nregions": nregions, "reg_off": reg_off}


class Store:
    """Distinct chunks, by id: their zlib bytes and raw length, and the order they were first seen in."""

    def __init__(self):
        self.seen = {}
        self.order = []

    def chunk(self, raw):
        cid = hashlib.sha256(raw).hexdigest()[:ID_HEX]
        if cid not in self.seen:
            self.seen[cid] = (zlib.compress(raw, 9), len(raw))
            self.order.append(cid)
        return cid


def write_packs(out, store, ids, pack_max):
    """Packs the chunks ids (in that order, each once) into packs of at most pack_max bytes."""
    packs, cur, cur_bytes = [], [], 0

    def flush():
        nonlocal cur, cur_bytes
        if not cur:
            return
        blob = b"".join(store.seen[c][0] for c in cur)
        pid = hashlib.sha256(blob).hexdigest()[:ID_HEX]
        name = "packs/%s.bin" % pid
        with open(os.path.join(out, name), "wb") as o:
            o.write(blob)
        at, rows = 0, []
        for c in cur:
            z, raw = store.seen[c]
            rows.append([c, at, len(z), raw])
            at += len(z)
        packs.append({"file": name, "bytes": len(blob), "chunks": rows})
        cur, cur_bytes = [], 0

    done = set()
    for c in ids:
        if c in done:
            continue
        done.add(c)
        z = store.seen[c][0]
        if cur and cur_bytes + len(z) > pack_max:
            flush()
        cur.append(c)
        cur_bytes += len(z)
    flush()
    return packs


def runs(pages):
    """[address, length] runs of consecutive page addresses (sorted)."""
    out = []
    for a in pages:
        if out and out[-1][0] + out[-1][1] == a:
            out[-1][1] += PAGE
        else:
            out.append([a, PAGE])
    return out


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("snapshot")
    ap.add_argument("out")
    ap.add_argument("--tree", required=True, help="the tree manifest the snapshot's engine booted")
    ap.add_argument("--chunk-size", type=int, default=262144)
    ap.add_argument("--pack-max", type=int, default=4 * 1024 * 1024)
    ap.add_argument("--eager", action="store_true", help="no lazy pages: the whole image before main")
    ap.add_argument("--order", help="a recorded touch order (tools/snapshot-web.mjs record)")
    ap.add_argument("--hot-ms", type=float, default=None,
                    help="pages the order touched up to this time after the restore are hot (default: its first frame)")
    ap.add_argument("--lazy-pages", type=int, default=32, help="pages per lazy chunk")
    ap.add_argument("--lazy-pack-max", type=int, default=1024 * 1024)
    a = ap.parse_args(argv)
    os.makedirs(os.path.join(a.out, "packs"), exist_ok=True)
    old_files = set()
    try:
        with open(os.path.join(a.out, "snapshot.json")) as o:
            old = json.load(o)
            old_files = {p["file"] for p in old["packs"]}
            old_files |= {p["file"] for p in old.get("prefetch", {}).get("packs", [])}  # tools/prefetch.py's
            if old.get("lazy"):
                old_files |= {p["file"] for p in old["lazy"]["packs"]}
                old_files.add(old["lazy"]["map"]["file"])
        with open(a.tree) as o:
            old_files -= {p["file"] for p in json.load(o).get("packs", [])}
    except (OSError, ValueError, KeyError):
        pass
    with open(a.tree, "rb") as t:
        tree = hashlib.sha256(t.read()).hexdigest()

    hot = Store()
    with open(a.snapshot, "rb") as f:
        h = header(f)
        f.seek(0)
        head = f.read(h["ext_off"])
        head_ids = [hot.chunk(head[i:i + a.chunk_size]) for i in range(0, len(head), a.chunk_size)]
        f.seek(h["ext_off"])
        table = f.read(16 * h["nextents"])
        regions = []
        if h["nregions"]:
            f.seek(h["reg_off"])
            rt = f.read(24 * h["nregions"])
            regions = sorted(struct.unpack_from("<QQQ", rt, 24 * i) for i in range(h["nregions"]))
        pool = [(r[0], r[0] + r[1]) for r in regions if r[2] == REG_POOL]
        blob = [(r[0], r[0] + r[1]) for r in regions if r[2] == REG_GL_BLOB]
        pool_lo = [p[0] for p in pool]
        lazy_on = bool(pool) and not a.eager

        def in_pool(addr):
            i = bisect.bisect_right(pool_lo, addr) - 1
            return i >= 0 and addr + PAGE <= pool[i][1]

        def kind(addr):
            if in_pool(addr):
                return "pool"
            if any(lo <= addr < hi for lo, hi in blob):
                return "gl_blob"
            return "engine"

        # Pages that the recorded order says are hot, and the touch order of the others.
        hot_pages, touch = set(), []
        if a.order and lazy_on:
            with open(a.order) as o:
                rec = json.load(o)
            cut = a.hot_ms if a.hot_ms is not None else rec.get("first_frame_ms", 0)
            for addr, t in rec["pages"]:
                if t <= cut:
                    hot_pages.add(addr)
                else:
                    touch.append(addr)

        classes = {}

        def account(k, raw):
            c = classes.setdefault(k, [0, 0])
            c[0] += len(raw)
            c[1] += len(zlib.compress(raw, 6))

        extents, lazy_bytes = [], {}  # hot extents; lazy page -> the file offset of its bytes
        f.seek(h["data_off"])
        for k in range(h["nextents"]):
            addr, length = struct.unpack_from("<QQ", table, 16 * k)
            data_at = f.tell()
            data = f.read(length)
            # Page by page (an extent's first and last piece may be partial): its kind for the census, and the hot
            # runs of the extent (everything but lazy pool pages).
            cut_runs, run_at = [], None
            kind_at, kind_of = 0, None
            pos = 0
            while pos < length:
                pa = addr + pos
                nxt = min(length, (pa // PAGE + 1) * PAGE - addr)
                kk = kind(pa)
                if kk != kind_of:
                    if kind_of is not None:
                        account(kind_of, data[kind_at:pos])
                    kind_at, kind_of = pos, kk
                lazy = lazy_on and kk == "pool" and pa % PAGE == 0 and nxt - pos == PAGE and pa not in hot_pages
                if lazy:
                    lazy_bytes[pa] = data_at + pos
                    if run_at is not None:
                        cut_runs.append((run_at, pos))
                        run_at = None
                elif run_at is None:
                    run_at = pos
                pos = nxt
            if kind_of is not None:
                account(kind_of, data[kind_at:length])
            if run_at is not None:
                cut_runs.append((run_at, length))
            for lo, hi in cut_runs:
                ids, p = [], lo
                while p < hi:
                    n = min(a.chunk_size, hi - p)
                    ids.append(hot.chunk(data[p:p + n]))
                    p += n
                extents.append([addr + lo, hi - lo, ids])

    hot_packs = write_packs(a.out, hot, hot.order, a.pack_max)
    m = {"version": 2 if lazy_on else 1, "format": "xiaoji-snapshot", "build": h["build"], "tree": tree,
         "chunk_size": a.chunk_size, "id_hex": ID_HEX, "icount": h["icount"],
         "head": {"bytes": len(head), "chunks": head_ids}, "ranges": h["ranges"], "extents": extents,
         "packs": hot_packs, "image_bytes": h["image_bytes"],
         "download_bytes": sum(p["bytes"] for p in hot_packs),
         "classes": {k: {"raw": v[0], "zlib6": v[1]} for k, v in sorted(classes.items())}}
    new_files = {p["file"] for p in hot_packs}
    if lazy_on and lazy_bytes:
        seen = set()
        order = []
        for pa in touch:
            if pa in lazy_bytes and pa not in seen:
                seen.add(pa)
                order.append(pa)
        touched = len(order)
        order += sorted(pa for pa in lazy_bytes if pa not in seen)
        lz = Store()
        ids = []
        cp = a.lazy_pages
        with open(a.snapshot, "rb") as f:
            def page(pa):
                f.seek(lazy_bytes[pa])
                return f.read(PAGE)
            for i in range(0, len(order), cp):
                ids.append(lz.chunk(b"".join(page(pa) for pa in order[i:i + cp])))
        # A lazy chunk is named by its bytes, but two chunks of equal bytes hold different pages: each is its own row.
        lazy_packs, cur, cur_bytes = [], [], 0

        def flush():
            nonlocal cur, cur_bytes
            if not cur:
                return
            blob = b"".join(lz.seen[c][0] for c in cur)
            pid = hashlib.sha256(blob).hexdigest()[:ID_HEX]
            name = "packs/%s.bin" % pid
            with open(os.path.join(a.out, name), "wb") as o:
                o.write(blob)
            at, rows = 0, []
            for c in cur:
                z, raw = lz.seen[c]
                rows.append([c, at, len(z), raw])
                at += len(z)
            lazy_packs.append({"file": name, "bytes": len(blob), "chunks": rows})
            cur, cur_bytes = [], 0

        for c in ids:
            z = lz.seen[c][0]
            if cur and cur_bytes + len(z) > a.lazy_pack_max:
                flush()
            cur.append(c)
            cur_bytes += len(z)
        flush()
        mz = zlib.compress(struct.pack("<%dI" % len(order), *[pa // PAGE for pa in order]), 9)
        mname = "packs/%s.map" % hashlib.sha256(mz).hexdigest()[:ID_HEX]
        with open(os.path.join(a.out, mname), "wb") as o:
            o.write(mz)
        m["lazy"] = {"pages": len(order), "page_bytes": PAGE, "chunk_pages": cp, "touched": touched,
                     "map": {"file": mname, "bytes": len(mz)}, "packs": lazy_packs,
                     "bytes": sum(p["bytes"] for p in lazy_packs), "raw_bytes": len(order) * PAGE}
        new_files |= {p["file"] for p in lazy_packs} | {mname}
        m["download_bytes"] += len(mz)
    tmp = os.path.join(a.out, "snapshot.json.tmp")
    with open(tmp, "w") as o:
        json.dump(m, o, separators=(",", ":"))
    os.replace(tmp, os.path.join(a.out, "snapshot.json"))
    for f in old_files - new_files:
        try:
            os.remove(os.path.join(a.out, f))
        except OSError:
            pass
    print("snappack: %s: %d MiB of memory, %d hot extents, %d distinct hot chunks, %d packs, %.1f MiB before main" % (
        h["build"], h["image_bytes"] >> 20, len(extents), len(hot.order), len(hot_packs),
        m["download_bytes"] / 1048576.0))
    if m.get("lazy"):
        lzm = m["lazy"]
        print("snappack: lazy: %d pages (%d MiB raw, %d touched in the order), %d packs, %.1f MiB after main" % (
            lzm["pages"], lzm["raw_bytes"] >> 20, lzm["touched"], len(lzm["packs"]), lzm["bytes"] / 1048576.0))
    for k, v in sorted(classes.items()):
        print("snappack: class %-8s %8.1f MiB raw, %7.1f MiB zlib-6" % (k, v[0] / 1048576.0, v[1] / 1048576.0))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
