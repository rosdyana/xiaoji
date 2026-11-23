#!/usr/bin/env python3
"""Packs the product tree into the web chunk store (docs/loading.md, tier 1). Stdlib only.

Usage: pack.py ROOTFS OUT_DIR --order ORDER.json [--chunk-size 65536] [--pack-max 16777216]

ROOTFS is the product tree (build/rootfs-xiaoji). OUT_DIR receives:

  chunks/<id>.z          each distinct chunk, zlib-compressed (level 9). <id> is the first 32 hex digits of
                         the SHA-256 of the raw chunk. Chunks are shared between files and are immutable.
  packs/<id>.bin         boot packs: the chunks of the boot's working set, in first-read order, concatenated,
                         each as its own zlib stream. Each pack is at most --pack-max bytes, so it fits a
                         Cloudflare Pages file (25 MiB). The manifest lists each chunk's offset in its pack.
  manifest-xiaoji-v2.json
                         the entries of manifest-xiaoji.json (dirs, symlinks, files, with the file size and the
                         chunk ids of each file), the packs and the chunk size. The browser needs no HEAD
                         request: the size comes from here.

ORDER.json is the boot's first-read order: [[guest path, 64 KiB chunk index], ...], written by
tools/workingset.py --order-out from the access trace. Each entry covers the bytes of one 64 KiB block, so it
maps to the chunks of any --chunk-size (one chunk when the size is 64 KiB or more, several when it is smaller).

The packer checks the deployed file count (chunks, packs and the manifest) against --max-files (20,000 is the
Cloudflare Pages limit per deployment) and fails above it. tests/chunks.mjs checks the output.
"""
import argparse
import hashlib
import json
import os
import sys
import zlib
from concurrent.futures import ProcessPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkmanifest  # noqa: E402  (the same tree walk as the page's manifest)

ID_HEX = 32
ORDER_BLOCK = 65536  # the granularity of the boot order (tools/workingset.py)
REQUIRED = ["/system/bin/init", "/linkerconfig", "/apex/apex-info-list.xml", "/system/framework/arm64/boot.art"]


def chunk_id(raw):
    return hashlib.sha256(raw).hexdigest()[:ID_HEX]


def deflate(job):
    raw, cid = job
    z = zlib.compress(raw, 9)
    return cid, z


def write_atomic(path, data):
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("rootfs")
    ap.add_argument("out")
    ap.add_argument("--order", required=True)
    ap.add_argument("--chunk-size", type=int, default=65536)
    ap.add_argument("--pack-max", type=int, default=16 * 1024 * 1024)
    ap.add_argument("--max-files", type=int, default=20000)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    a = ap.parse_args(argv[1:])
    cs = a.chunk_size
    if cs <= 0 or cs % 4096:
        ap.error("--chunk-size must be a positive multiple of 4096")
    if a.pack_max > 25 * 1024 * 1024:
        ap.error("--pack-max must stay under the 25 MiB file limit")

    entries, _skipped = mkmanifest.walk(a.rootfs, product=True)
    have = {e["path"] for e in entries}
    missing = [p for p in REQUIRED if p not in have]
    if missing:
        print("pack: missing from rootfs: " + ", ".join(missing), file=sys.stderr)
        return 1

    chunk_dir = os.path.join(a.out, "chunks")
    pack_dir = os.path.join(a.out, "packs")
    os.makedirs(chunk_dir, exist_ok=True)
    os.makedirs(pack_dir, exist_ok=True)

    stored = set(f[:-2] for f in os.listdir(chunk_dir) if f.endswith(".z"))
    new_chunks = 0
    raw_total = 0
    z_total = 0
    batch, batch_bytes = [], 0
    pool = ProcessPoolExecutor(max_workers=a.jobs)

    def flush():
        nonlocal batch, batch_bytes, new_chunks, z_total
        for cid, z in pool.map(deflate, batch, chunksize=8):
            write_atomic(os.path.join(chunk_dir, cid + ".z"), z)
            stored.add(cid)
            new_chunks += 1
            z_total += len(z)
        batch, batch_bytes = [], 0

    file_chunks = {}  # guest path -> [chunk id, ...]
    sizes = {e["path"]: e["size"] for e in entries if e["type"] == "file"}
    for e in entries:
        if e["type"] != "file":
            continue
        ids = []
        with open(os.path.join(a.rootfs, e["path"][1:]), "rb") as f:
            size = 0
            while True:
                raw = f.read(cs)
                if not raw:
                    break
                size += len(raw)
                cid = chunk_id(raw)
                ids.append(cid)
                raw_total += len(raw)
                if cid in stored:  # on disk already, or queued in this run
                    continue
                batch.append((raw, cid))
                batch_bytes += len(raw)
                stored.add(cid)  # reserved: a repeat in this batch is not queued twice
                if batch_bytes >= 64 * 1024 * 1024:
                    flush()
        if size != e["size"]:
            print(f"pack: {e['path']} changed size while packing", file=sys.stderr)
            return 1
        file_chunks[e["path"]] = ids
    flush()
    pool.shutdown()

    # Boot packs: the first-read order of the working set, deduplicated, in packs of at most --pack-max bytes.
    with open(a.order) as f:
        order = json.load(f)
    by_path_chunk = {}
    packs = []
    cur, cur_bytes, cur_index = [], 0, []
    seen = set()

    def close_pack():
        nonlocal cur, cur_bytes, cur_index
        if not cur:
            return
        blob = b"".join(cur)
        name = hashlib.sha256(blob).hexdigest()[:ID_HEX] + ".bin"
        write_atomic(os.path.join(pack_dir, name), blob)
        packs.append({"file": "packs/" + name, "bytes": len(blob), "chunks": cur_index})
        cur, cur_bytes, cur_index = [], 0, []

    for path, block in order:
        guest = path[len("/rootfs"):] if path.startswith("/rootfs/") else path
        ids = file_chunks.get(guest)
        if ids is None:
            continue  # the trace names a file that is no longer in the tree
        first = block * ORDER_BLOCK // cs
        last = min((block + 1) * ORDER_BLOCK - 1, sizes[guest] - 1) // cs
        for idx in range(first, min(last, len(ids) - 1) + 1):
            cid = ids[idx]
            if cid in seen:
                continue
            seen.add(cid)
            with open(os.path.join(chunk_dir, cid + ".z"), "rb") as f:
                z = f.read()
            raw_len = min(cs, sizes[guest] - idx * cs)
            if cur_bytes + len(z) > a.pack_max:
                close_pack()
            cur_index.append([cid, cur_bytes, len(z), raw_len])
            cur.append(z)
            cur_bytes += len(z)
    close_pack()

    used = {cid for ids in file_chunks.values() for cid in ids}
    for name in os.listdir(chunk_dir):  # chunks of an older tree are not deployed
        if name.endswith(".z") and name[:-2] not in used:
            os.unlink(os.path.join(chunk_dir, name))
    keep = {p["file"][len("packs/"):] for p in packs}
    for name in os.listdir(pack_dir):
        if name not in keep:
            os.unlink(os.path.join(pack_dir, name))
    manifest_entries = []
    for e in entries:
        if e["type"] == "file":
            manifest_entries.append({"path": e["path"], "type": "file", "mode": e["mode"], "size": e["size"],
                                     "chunks": file_chunks[e["path"]]})
        else:
            manifest_entries.append(e)
    doc = {
        "version": 2,
        "format": "xiaoji-chunks-1",
        "chunk_size": cs,
        "id_hex": ID_HEX,
        "count": len(manifest_entries),
        "chunks": len(used),
        "packs": packs,
        "entries": manifest_entries,
    }
    man_path = os.path.join(a.out, "manifest-xiaoji-v2.json")
    write_atomic(man_path, (json.dumps(doc, separators=(",", ":")) + "\n").encode())

    deployed = len(used) + len(packs) + 1
    pack_bytes = sum(p["bytes"] for p in packs)
    print(json.dumps({
        "chunk_size": cs,
        "files": sum(1 for e in entries if e["type"] == "file"),
        "raw_mb_packed": round(raw_total / 2**20, 1),
        "chunks": len(used),
        "new_chunks": new_chunks,
        "chunks_zlib_mb": round(z_total / 2**20, 1),
        "packs": len(packs),
        "pack_mb": round(pack_bytes / 2**20, 2),
        "deployed_files": deployed,
    }))
    if deployed > a.max_files:
        print(f"pack: {deployed} files is over the {a.max_files} limit; use a larger --chunk-size", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
