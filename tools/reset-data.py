#!/usr/bin/env python3
"""Clear the writable /data of an xiaoji rootfs, and its guest metadata records for /data (docs/boot.md, B8).

usage: python3 -I tools/reset-data.py ROOTFS

ROOTFS must be a directory named rootfs* directly under the repository's build/ (for example build/rootfs-xiaoji).
Every entry under ROOTFS/data is removed (the data directory itself stays). In ROOTFS/.xiaoji-meta, the lines for
/data and below are dropped, so the next boot starts from no records there. Other trees are left as they are.
The removal is done here with os and shutil, one path at a time, and only inside ROOTFS/data.
"""
import os
import shutil
import sys

REPO = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
SIDECAR = ".xiaoji-meta"
HEADER = "xiaoji-meta 1"


def refuse(msg):
    print("reset-data: " + msg, file=sys.stderr)
    return 2


def drop_data_records(root):
    path = os.path.join(root, SIDECAR)
    if not os.path.isfile(path) or os.path.islink(path):
        return
    with open(path, "r") as f:
        lines = f.read().split("\n")
    if not lines or lines[0] != HEADER:
        return  # not a sidecar of this version: leave it alone
    kept = [lines[0]]
    for line in lines[1:]:
        if not line:
            continue
        guest = line.split("\t", 1)[0]
        if guest == "/data" or guest.startswith("/data/"):
            continue
        kept.append(line)
    tmp = os.path.join(root, SIDECAR + ".tmp")
    with open(tmp, "w") as f:
        f.write("\n".join(kept) + "\n")
    os.replace(tmp, path)


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip().split("\n")[0], file=sys.stderr)
        print("usage: python3 -I tools/reset-data.py ROOTFS", file=sys.stderr)
        return 2
    root = os.path.realpath(argv[1])
    build = os.path.realpath(os.path.join(REPO, "build"))
    if os.path.dirname(root) != build or not os.path.basename(root).startswith("rootfs"):
        return refuse("refusing %s: it must be a rootfs* directory directly under %s" % (root, build))
    if not os.path.isdir(root):
        return refuse("no such rootfs: %s" % root)
    data = os.path.join(root, "data")
    if os.path.islink(data):
        return refuse("refusing: %s is a symbolic link" % data)
    if os.path.isdir(data):
        for name in os.listdir(data):
            p = os.path.join(data, name)
            if os.path.isdir(p) and not os.path.islink(p):
                shutil.rmtree(p)
            else:
                os.unlink(p)
    drop_data_records(root)
    print("reset-data: cleared %s and its /data records" % data)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
