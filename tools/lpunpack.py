#!/usr/bin/env python3
"""Extract the logical partitions of an Android dynamic-partition ("super") image.

Usage: lpunpack.py DISK_OR_SUPER OUT_DIR [--offset BYTES]

DISK_OR_SUPER is either a raw super image or a disk image with the super
partition starting at --offset bytes (for the emulator's GPT system.img, pass
the super partition's start sector * 512). Each logical partition is written
as OUT_DIR/<name>.img. Only metadata slot 0 is read; zero-target extents are
written as holes.

Format: system/core/fs_mgr/liblp/include/liblp/metadata_format.h (AOSP).
"""
import argparse, os, struct, sys

GEOMETRY_MAGIC = 0x616C4467  # "gDla"
HEADER_MAGIC = 0x414C5030    # "0PLA"
RESERVED = 4096              # bytes before the geometry
GEOMETRY_SIZE = 4096         # geometry block, stored twice
SECTOR = 512

ap = argparse.ArgumentParser()
ap.add_argument("image")
ap.add_argument("out")
ap.add_argument("--offset", type=int, default=0)
a = ap.parse_args()

f = open(a.image, "rb")


def read(off, n):
    f.seek(a.offset + off)
    b = f.read(n)
    if len(b) != n:
        sys.exit(f"short read at {off}")
    return b


magic, _size = struct.unpack_from("<II", read(RESERVED, 8))
if magic != GEOMETRY_MAGIC:
    sys.exit(f"no LP geometry at offset {a.offset + RESERVED} (magic {magic:#x})")
geo = read(RESERVED, 52)
metadata_max_size, slot_count, block_size = struct.unpack_from("<III", geo, 40)

meta_off = RESERVED + 2 * GEOMETRY_SIZE  # primary metadata, slot 0
hdr = read(meta_off, 256)
(hmagic, major, minor, header_size) = struct.unpack_from("<IHHI", hdr, 0)
if hmagic != HEADER_MAGIC:
    sys.exit(f"bad metadata header magic {hmagic:#x}")
tables_size = struct.unpack_from("<I", hdr, 44)[0]
# Table descriptors (offset, num_entries, entry_size) start after tables_checksum.
descs = [struct.unpack_from("<III", hdr, 80 + 12 * i) for i in range(4)]
(p_off, p_n, p_sz), (e_off, e_n, e_sz), (g_off, g_n, g_sz), (b_off, b_n, b_sz) = descs
tables = read(meta_off + header_size, tables_size)

extents = []
for i in range(e_n):
    num_sectors, target_type, target_data, _src = struct.unpack_from("<QIQI", tables, e_off + i * e_sz)
    extents.append((num_sectors, target_type, target_data))

os.makedirs(a.out, exist_ok=True)
print(f"LP metadata v{major}.{minor}, block size {block_size}, {p_n} partitions")
for i in range(p_n):
    raw = tables[p_off + i * p_sz: p_off + (i + 1) * p_sz]
    name = raw[:36].split(b"\0", 1)[0].decode()
    _attrs, first, count, _group = struct.unpack_from("<IIII", raw, 36)
    total = sum(extents[j][0] for j in range(first, first + count)) * SECTOR
    print(f"  {name:20s} {total / 2**20:9.1f} MiB  {count} extent(s)")
    if total == 0:
        continue
    with open(os.path.join(a.out, name + ".img"), "wb") as out:
        for j in range(first, first + count):
            num_sectors, ttype, tdata = extents[j]
            n = num_sectors * SECTOR
            if ttype == 1:  # zero
                out.seek(n, os.SEEK_CUR)
                continue
            f.seek(a.offset + tdata * SECTOR)
            left = n
            while left:
                chunk = f.read(min(left, 8 << 20))
                if not chunk:
                    sys.exit(f"{name}: extent past end of image")
                out.write(chunk)
                left -= len(chunk)
        out.truncate()
