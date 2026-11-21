#!/usr/bin/env python3
"""Pull the filesystem payload out of each APEX in a directory.

Usage: apexunpack.py APEX_DIR OUT_DIR

For every NAME.apex or NAME.capex in APEX_DIR, writes OUT_DIR/NAME.img (the
apex_payload.img, an ext4 or EROFS image) and OUT_DIR/NAME.manifest.pb.
A .capex is a zip holding original_apex, which is itself the .apex zip.
Mounting the payload at /apex/NAME is the caller's job (debugfs rdump).
"""
import io, os, sys, zipfile


def payload_of(path):
    z = zipfile.ZipFile(path)
    names = set(z.namelist())
    if "original_apex" in names:  # compressed APEX
        z = zipfile.ZipFile(io.BytesIO(z.read("original_apex")))
        names = set(z.namelist())
    if "apex_payload.img" not in names:
        raise ValueError(f"no apex_payload.img in {path}")
    manifest = z.read("apex_manifest.pb") if "apex_manifest.pb" in names else b""
    return z, manifest


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for fn in sorted(os.listdir(src)):
        stem, ext = os.path.splitext(fn)
        if ext not in (".apex", ".capex"):
            continue
        z, manifest = payload_of(os.path.join(src, fn))
        with z.open("apex_payload.img") as i, open(os.path.join(out, stem + ".img"), "wb") as o:
            while chunk := i.read(8 << 20):
                o.write(chunk)
        with open(os.path.join(out, stem + ".manifest.pb"), "wb") as o:
            o.write(manifest)
        print(stem)


main()
