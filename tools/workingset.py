#!/usr/bin/env python3
"""Working-set analysis of a Boot Android access trace (docs/loading.md, step 1).

Usage: workingset.py TRACE.json [TRACE.json ...] ROOTFS-XIAOJI [--order-out ORDER.json]

With --order-out, writes the boot's first-read order of 64 KiB chunks ([[path, chunk], ...]) for tools/pack.py.
With several traces the order is their union (the report is for the first trace).

TRACE.json is the "trace" message of tools/webboot-bench.mjs --trace (web/trace.js): per file the
4 KiB pages and 64 KiB chunks that the engine's reads touched, and the first-read order of the chunks.
ROOTFS-XIAOJI is the product tree the page read (build/rootfs-xiaoji). Prints a JSON report to stdout.

Reports:
  - bytes touched at 4 KiB and 64 KiB granularity, the size of the files they lie in, and the bytes of
    chunks of several sizes that cover the touched pages (the over-fetch of each chunk size);
  - compressibility of the touched 64 KiB chunks, overall and by file type: gzip (zlib, stdlib), and
    brotli and zstd through their command-line tools (if present), each as one solid stream per type
    and, for gzip, per chunk (what a content-addressed store would ship).
"""
import json
import os
import shutil
import subprocess
import sys
import zlib

PAGE = 4096
CHUNK = 65536
CHUNK_SIZES = [16384, 65536, 131072, 262144, 1048576]


def kind(path):
    base = path.rsplit("/", 1)[-1]
    if ".so" in base:
        return "so"
    ext = base.rsplit(".", 1)[-1] if "." in base else ""
    if ext in ("oat", "odex", "vdex", "art"):
        return "oat/odex/vdex/art"
    if ext in ("jar", "apk", "apex"):
        return "jar/apk"
    return "other"


def solid(data, tool, level):
    if tool == "gzip":
        return len(zlib.compress(data, level))
    exe = shutil.which(tool)
    if not exe:
        return None
    args = [exe, "-c", "-q", str(level)] if tool == "brotli" else [exe, "-c", "-q", f"-{level}"]
    out = subprocess.run(args, input=data, stdout=subprocess.PIPE, check=True).stdout
    return len(out)


def main(argv):
    args = argv[1:]
    order_out = None
    if "--order-out" in args:
        i = args.index("--order-out")
        order_out = args[i + 1]
        del args[i:i + 2]
    if len(args) < 2:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    *trace_paths, root = args
    traces = []
    for trace_path in trace_paths:
        with open(trace_path) as f:
            tr = json.load(f)
        tr = tr.get("trace", tr)  # tools/webboot-bench.mjs output keeps the page message under "trace"
        traces.append(tr["access"])
    acc = traces[0]  # the report is for the first trace; the order is the union of all of them
    files = acc["files"]

    total_fs = 0
    touched_pages_bytes = 0
    touched_chunk64_bytes = 0
    touched_file_bytes = 0
    per_kind = {}
    chunk_blobs = {}  # kind -> list of chunk bytes (64 KiB granularity)
    overfetch = {str(c): {"chunks": 0, "bytes": 0} for c in CHUNK_SIZES}
    page_bytes_all = 0

    for path, info in files.items():
        size = info["size"]
        total_fs += size
        touched_file_bytes += size
        k = kind(path)
        pk = per_kind.setdefault(k, {"files": 0, "file_bytes": 0, "page_bytes": 0, "chunk64_bytes": 0})
        pk["files"] += 1
        pk["file_bytes"] += size
        pages = set()
        for start, count in info["pages"]:
            pages.update(range(start, start + count))
        page_bytes_all += len(pages) * PAGE
        pk["page_bytes"] += len(pages) * PAGE
        for c in CHUNK_SIZES:
            per_pg = c // PAGE
            touched = {p // per_pg for p in pages}
            overfetch[str(c)]["chunks"] += len(touched)
            overfetch[str(c)]["bytes"] += sum(min(c, size - i * c) for i in touched if i * c < size)
        chunks = set()
        for start, count in info["chunks"]:
            chunks.update(range(start, start + count))
        pk["chunk64_bytes"] += len(chunks) * CHUNK
        touched_chunk64_bytes += sum(min(CHUNK, size - i * CHUNK) for i in chunks)
        if chunks:
            host = os.path.join(root, path[len("/rootfs/"):])
            with open(host, "rb") as f:
                for i in sorted(chunks):
                    f.seek(i * CHUNK)
                    chunk_blobs.setdefault(k, []).append(f.read(CHUNK))

    report = {
        "trace": {"calls": acc["calls"], "bytes_read": acc["bytes"], "read_ms": round(acc["readMs"]),
                  "files": len(files), "first_touch_chunks": len(acc["order"])},
        "sizes": {
            "touched_files_total_bytes": touched_file_bytes,
            "touched_4k_pages_bytes": page_bytes_all,
            "touched_64k_chunks_bytes": touched_chunk64_bytes,
        },
        "by_kind": per_kind,
        "chunk_size_overfetch": overfetch,
        "compress": {},
    }
    all_blobs = [b for lst in chunk_blobs.values() for b in lst]
    raw_total = sum(len(b) for b in all_blobs)
    report["compress"]["raw_bytes"] = raw_total

    levels = {"gzip": [1, 6, 9], "brotli": [5, 9], "zstd": [3, 15]}
    for tool, lvls in levels.items():
        for lv in lvls:
            if tool != "gzip" and shutil.which(tool) is None:
                report["compress"][f"{tool}-{lv}"] = "tool missing"
                continue
            entry = {}
            for k, lst in chunk_blobs.items():
                data = b"".join(lst)
                n = solid(data, tool, lv)
                entry[k] = {"raw": len(data), "solid": n}
            total = sum(e["solid"] for e in entry.values())
            entry["all"] = {"raw": raw_total, "solid": total}
            if tool == "gzip":
                per_chunk = sum(len(zlib.compress(b, lv)) for b in all_blobs)
                entry["all"]["per_chunk_64k"] = per_chunk
            report["compress"][f"{tool}-{lv}"] = entry
    if order_out:
        union, seen = [], set()
        for t in traces:  # first-read order: the first trace's entries, then the new ones of each later trace
            for p, c in t["order"]:
                if (p, c) not in seen:
                    seen.add((p, c))
                    union.append([p, c])
        with open(order_out, "w") as f:
            json.dump(union, f, separators=(",", ":"))
            f.write("\n")
    print(json.dumps(report, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
