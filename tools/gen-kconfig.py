#!/usr/bin/env python3
"""The kernel configuration the engine reports as /proc/config.gz (B18).

  tools/gen-kconfig.py <root> <x.y> <out.inc> [android-release]

Reads the image's device manifest (<root>/vendor/etc/vintf/manifest.xml: its <kernel target-level>, the kernel FCM
level) and framework compatibility matrices (<root>/system/etc/vintf/compatibility_matrix.<level>.xml), takes the
<kernel> entries for kernel x.y from the lowest-level matrix at or above the kernel FCM level that has them (what
libvintf's KernelInfo::getMatchedKernelRequirements picks). An x.y whose lowest matrix is above the kernel FCM level is
refused, as libvintf refuses it ("the first kernel FCM version allowed for kernel version x.y is N"). It writes the config lines they require as a C string literal, plus the minimum LTS
release as XJ_KCONFIG_LTS. Conditional entries are applied as libvintf applies them: an entry's <conditions> are
checked against the configuration itself, so the result is computed to a fixed point from a few facts about the
machine the engine presents (an arm64 device-tree kernel with ext4; see FACTS). Every requirement must agree with
every other; a conflict stops the script.

With an Android release (13), the release is a GKI one, <lts>-android13-<gen>: libvintf (RuntimeInfo::
parseGkiKernelRelease, gkiAndroidReleaseToLevel) takes the kernel FCM level from it (Android 12 is level 6, so 13 is 7)
instead of from the device manifest, and VintfObject only warns when the manifest says otherwise. The generator then
uses that level, and writes XJ_KCONFIG_SUFFIX ("-android13-<gen>") for the engine's release string.

The output is checked in (engine/kconfig.inc), so the engine builds without the image; rerun this after the image's
matrices or manifest change:
  tools/gen-kconfig.py build/rootfs-xiaoji 5.15 engine/kconfig.inc 13
"""
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

# What the engine presents: arm64 (no 32-bit ARM, no x86), described by a device tree, not ACPI, with ext4
# (/proc/filesystems), vmap stacks and zero-initialised stacks (conditions of the newer matrices; facts either way).
FACTS = {
    'CONFIG_ARM64': 'y',
    'CONFIG_OF': 'y',
    'CONFIG_EXT4_FS': 'y',
    'CONFIG_HAVE_ARCH_VMAP_STACK': 'y',
    'CONFIG_CC_HAS_AUTO_VAR_INIT_ZERO': 'y',
}


def value_of(cfg, key):
    """A config's value as libvintf reads it: an absent tristate is n."""
    return cfg.get(key, 'n')


def kernel_level(root):
    m = ET.parse(Path(root) / 'vendor/etc/vintf/manifest.xml').getroot()
    k = m.find('kernel')
    if k is None or not (k.get('target-level') or '').isdigit():
        sys.exit('gen-kconfig: the device manifest has no <kernel target-level>')
    return int(k.get('target-level'))


def entries(root, xy, klevel):
    found = []
    for f in sorted(Path(root, 'system/etc/vintf').glob('compatibility_matrix.*.xml')):
        r = ET.parse(f).getroot()
        level = r.get('level')
        if r.get('type') != 'framework' or not level or not level.isdigit():
            continue
        ks = [k for k in r.findall('kernel') if '.'.join(k.get('version').split('.')[:2]) == xy]
        if ks:
            found.append((int(level), f.name, ks))
    found.sort()
    if not found:
        sys.exit(f'gen-kconfig: no matrix has kernel {xy}')
    found = [e for e in found if e[0] >= klevel] if found[0][0] < klevel else found
    if not found or found[0][0] != klevel:
        first = found[0][0] if found else None
        sys.exit(f'gen-kconfig: kernel {xy} is first allowed at level {first}, the kernel FCM level is {klevel}: '
                 'libvintf refuses it')
    return found[0]


def main():
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    root, xy, out = sys.argv[1:4]
    android = int(sys.argv[4]) if len(sys.argv) == 5 else None
    GKI_GEN = 8  # the GKI generation in the release string (<lts>-android<N>-<gen>); any number parses
    klevel = android - 12 + 6 if android else kernel_level(root)
    level, fname, kernels = entries(root, xy, klevel)
    lts = {k.get('version') for k in kernels}
    if len(lts) != 1:
        sys.exit(f'gen-kconfig: {fname} has several minimum LTS releases for {xy}: {sorted(lts)}')
    blocks = []
    for k in kernels:
        conds = [(c.findtext('key'), c.findtext('value')) for c in k.findall('conditions/config')]
        reqs = []
        for c in k.findall('config'):
            v = c.find('value')
            reqs.append((c.findtext('key'), v.get('type'), (v.text or '').strip()))
        blocks.append((conds, reqs))

    cfg = dict(FACTS)
    applied = set()
    changed = True
    while changed:
        changed = False
        for i, (conds, reqs) in enumerate(blocks):
            if i in applied or any(value_of(cfg, key) != want for key, want in conds):
                continue
            applied.add(i)
            changed = True
            for key, typ, val in reqs:
                if typ == 'range':
                    val = val.split('-')[0]
                if key in cfg and cfg[key] != val:
                    sys.exit(f'gen-kconfig: {key} is required as {val!r} and as {cfg[key]!r}')
                cfg[key] = val if typ != 'string' else f'"{val}"'
    # Every block whose conditions hold now is applied (a later requirement cannot have made one hold).
    for i, (conds, _) in enumerate(blocks):
        if i not in applied and all(value_of(cfg, key) == want for key, want in conds):
            sys.exit(f'gen-kconfig: conditions {conds} hold but the block was not applied')

    lines = []
    for key in sorted(cfg):
        v = cfg[key]
        lines.append(f'# {key} is not set' if v == 'n' else f'{key}={v}')
    text = '\n'.join(lines) + '\n'
    with open(out, 'w') as o:
        source = 'from the GKI release' if android else "the device manifest's"
        o.write(f'/* Generated by tools/gen-kconfig.py from {fname} (level {level}; the kernel FCM level is {klevel}, '
                f'{source}), kernel {xy}: do not edit. */\n')
        o.write(f'#define XJ_KCONFIG_LTS "{lts.pop()}"\n')
        o.write(f'#define XJ_KCONFIG_SUFFIX "{"-android%d-%d" % (android, GKI_GEN) if android else ""}"\n')
        o.write('static const char xj_kconfig_text[] =\n')
        for line in lines:
            esc = line.replace('\\', '\\\\').replace('"', '\\"')
            o.write(f'    "{esc}\\n"\n')
        o.write('    ;\n')
    print(f'gen-kconfig: {len(lines)} lines from {fname} (level {level}), {len(applied)} of {len(blocks)} entries')


if __name__ == '__main__':
    main()
