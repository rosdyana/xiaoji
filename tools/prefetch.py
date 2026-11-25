#!/usr/bin/env python3
"""The restore's prefetch (docs/demo.md, P1; docs/snapshot.md). Stdlib only.

After a restore the tree's chunks are read on demand, synchronously, by the engine's worker: the clean file pages
(library code, the boot image, the apps' odex files) were dropped from the snapshot, so the first touch of each page
after the restore reads its chunk again. The prefetch reads the chunks a visitor's first minute needs (the home
screen, the drawer, the demo apps) in the background instead, in the order a recorded session first read them: a
second worker (web/prefetch.js) fetches them in small packs and the chunk store takes a chunk from there when it has
arrived (web/chunks.js, attachPrefetch).

  prefetch.py record FETCH_LOG... --tree MANIFEST --out ORDER
      The order: the chunk GETs of the page's server log (tools/latency.mjs --fetch-log; a restore reads only the
      tree's chunks this way), first read first, as [path, chunk index] pairs (image/restore-order.json), which name
      the same data in a rebuilt tree. Several logs: their union, in the first log's order, then the others' new ones.
  prefetch.py pack ORDER --xj DIR [--pack-max 2097152]
      Packs the order's chunks (the tree's chunk files, unchanged) into packs/<id>.bin of at most --pack-max bytes,
      and records them in DIR/snapshot.json as "prefetch": {packs, bytes, raw_bytes}, in the packs' format of the
      tree manifest ([id, offset, zlib bytes, raw bytes]). The packs of an earlier prefetch are removed.
"""
import argparse
import hashlib
import json
import os
import re
import sys

ID_HEX = 32
GET = re.compile(r"\b200 GET /xj/chunks/([0-9a-f]{%d})\.z\b" % ID_HEX)


def tree_index(manifest):
    """id -> (path, index), the first file that has the chunk; (path, index) -> (id, raw length)."""
    by_id, by_pos = {}, {}
    cs = manifest["chunk_size"]
    for e in manifest["entries"]:
        if e.get("type") != "file":
            continue
        for k, cid in enumerate(e.get("chunks", [])):
            raw = min(cs, e["size"] - k * cs)
            by_pos[(e["path"], k)] = (cid, raw)
            by_id.setdefault(cid, (e["path"], k))
    return by_id, by_pos


def record(a):
    with open(a.tree) as f:
        by_id, _ = tree_index(json.load(f))
    order, seen, unknown = [], set(), 0
    for log in a.logs:
        with open(log) as f:
            for line in f:
                m = GET.search(line)
                if not m:
                    continue
                cid = m.group(1)
                if cid in seen:
                    continue
                seen.add(cid)
                pos = by_id.get(cid)
                if pos is None:
                    unknown += 1
                    continue
                order.append([pos[0], pos[1]])
    tmp = a.out + ".tmp"
    with open(tmp, "w") as f:
        f.write("[\n" + ",\n".join(json.dumps(p) for p in order) + "\n]\n")
    os.replace(tmp, a.out)
    print("prefetch: %d chunks in the order (%d not in the tree) -> %s" % (len(order), unknown, a.out))
    return 0


def pack(a):
    xj = a.xj
    with open(os.path.join(xj, "manifest-xiaoji-v2.json")) as f:
        _, by_pos = tree_index(json.load(f))
    snap_path = os.path.join(xj, "snapshot.json")
    with open(snap_path) as f:
        snap = json.load(f)
    with open(a.order) as f:
        order = json.load(f)
    old = {p["file"] for p in snap.get("prefetch", {}).get("packs", [])}
    packs, cur, cur_rows, cur_bytes, seen, missing, raw_total = [], [], [], 0, set(), 0, 0

    def flush():
        nonlocal cur, cur_rows, cur_bytes
        if not cur:
            return
        blob = b"".join(cur)
        name = "packs/%s.bin" % hashlib.sha256(blob).hexdigest()[:ID_HEX]
        with open(os.path.join(xj, name), "wb") as o:
            o.write(blob)
        packs.append({"file": name, "bytes": len(blob), "chunks": cur_rows})
        cur, cur_rows, cur_bytes = [], [], 0

    for path, k in order:
        hit = by_pos.get((path, k))
        if hit is None:
            missing += 1  # the file or the chunk is no longer in the tree
            continue
        cid, raw = hit
        if cid in seen:
            continue
        seen.add(cid)
        with open(os.path.join(xj, "chunks", cid + ".z"), "rb") as f:
            z = f.read()
        if cur and cur_bytes + len(z) > a.pack_max:
            flush()
        cur_rows.append([cid, cur_bytes, len(z), raw])
        cur.append(z)
        cur_bytes += len(z)
        raw_total += raw
    flush()
    snap["prefetch"] = {"packs": packs, "bytes": sum(p["bytes"] for p in packs), "raw_bytes": raw_total}
    tmp = snap_path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(snap, f, separators=(",", ":"))
    os.replace(tmp, snap_path)
    used = {p["file"] for p in snap.get("packs", [])}
    for name in old - {p["file"] for p in packs} - used:
        try:
            os.remove(os.path.join(xj, name))
        except OSError:
            pass
    print("prefetch: %d chunks (%d not in the tree), %d packs, %.1f MiB to download, %.1f MiB raw" % (
        len(seen), missing, len(packs), snap["prefetch"]["bytes"] / 1048576.0, raw_total / 1048576.0))
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("record")
    r.add_argument("logs", nargs="+")
    r.add_argument("--tree", required=True)
    r.add_argument("--out", required=True)
    p = sub.add_parser("pack")
    p.add_argument("order")
    p.add_argument("--xj", required=True)
    p.add_argument("--pack-max", type=int, default=2 * 1024 * 1024)
    a = ap.parse_args(argv)
    return record(a) if a.cmd == "record" else pack(a)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
