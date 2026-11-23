// Checks the chunk store that tools/pack.py wrote (docs/loading.md). For every chunk: the file exists, it
// decodes with the page's own decoder (web/inflate.js), and its SHA-256 prefix is its name. For every file
// of the product tree: its chunks reassemble to the original bytes. For every boot pack: its bytes are the
// listed chunks, in order, and it has the manifest's size.
//
//   node tests/chunks.mjs [--build build/web/xj] [--rootfs build/rootfs-xiaoji] [--sample N]
//
// --sample N checks every pack and N files spread over the tree (the full check reads the whole tree).

import { createHash } from 'node:crypto';
import { existsSync, readFileSync, statSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { inflateZlib } from '../web/inflate.js';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');

function opt(name, dflt) {
  const i = process.argv.indexOf(name);
  return i >= 0 ? process.argv[i + 1] : dflt;
}

const build = resolve(ROOT, opt('--build', 'build/web/xj'));
const rootfs = resolve(ROOT, opt('--rootfs', 'build/rootfs-xiaoji'));
const sample = Number(opt('--sample', '0'));

let failures = 0;
function fail(msg) {
  failures++;
  if (failures <= 20) console.log('FAIL  ' + msg);
}

const sha = (bytes) => createHash('sha256').update(bytes).digest('hex');
const id = (bytes) => sha(bytes).slice(0, 32);

const manifestPath = join(build, 'manifest-xiaoji-v2.json');
if (!existsSync(manifestPath)) {
  console.error(`missing ${manifestPath}; run make build/web/xj/manifest-xiaoji-v2.json`);
  process.exit(2);
}
const man = JSON.parse(readFileSync(manifestPath, 'utf8'));
if (man.version !== 2 || !man.chunk_size) {
  console.error('not a v2 manifest');
  process.exit(2);
}
const cs = man.chunk_size;
const files = man.entries.filter((e) => e.type === 'file');

// Each chunk's raw length comes from the file that names it (chunks are full-size except a file's last).
const rawLen = new Map();
for (const f of files) {
  const want = Math.ceil(f.size / cs);
  if (f.chunks.length !== want) fail(`${f.path}: ${f.chunks.length} chunks, want ${want} for ${f.size} bytes`);
  for (let i = 0; i < f.chunks.length; i++) {
    rawLen.set(f.chunks[i], Math.min(cs, f.size - i * cs));
  }
}

// Decodes one chunk with web/inflate.js and checks its hash, once per chunk. Returns the raw bytes, or null.
const verified = new Set();
function chunkBytes(cid) {
  const p = join(build, 'chunks', cid + '.z');
  if (!existsSync(p)) {
    fail(`chunk ${cid} is missing`);
    return null;
  }
  const z = new Uint8Array(readFileSync(p));
  let raw;
  try {
    raw = inflateZlib(z, rawLen.get(cid));
  } catch (e) {
    fail(`chunk ${cid}: ${e.message}`);
    return null;
  }
  if (!verified.has(cid)) {
    if (id(raw) !== cid) fail(`chunk ${cid}: hash of the decoded bytes is ${id(raw)}`);
    verified.add(cid);
  }
  return raw;
}

// 2. Every boot pack: size, and each listed chunk is the chunk file's bytes at its offset.
for (const pk of man.packs) {
  const p = join(build, pk.file);
  if (!existsSync(p)) {
    fail(`pack ${pk.file} is missing`);
    continue;
  }
  const bytes = readFileSync(p);
  if (bytes.length !== pk.bytes) fail(`pack ${pk.file}: ${bytes.length} bytes, manifest says ${pk.bytes}`);
  if (id(bytes) !== pk.file.slice(pk.file.lastIndexOf('/') + 1, -4)) fail(`pack ${pk.file}: name is not its hash`);
  for (const [cid, off, len, raw] of pk.chunks) {
    const want = readFileSync(join(build, 'chunks', cid + '.z'));
    if (!bytes.subarray(off, off + len).equals(want)) fail(`pack ${pk.file}: chunk ${cid} differs from its file`);
    if (rawLen.get(cid) !== raw) fail(`pack ${pk.file}: chunk ${cid} raw length ${raw}`);
  }
}
console.log(`packs: ${man.packs.length} checked`);

// 3. Every chunk is decoded and hashed (through the files that name it), and every file reassembles from
// its chunks to the original bytes.
const pick = sample > 0 ? files.filter((_, i) => i % Math.max(1, Math.floor(files.length / sample)) === 0) : files;
let reassembled = 0;
let bytes = 0;
for (const f of pick) {
  const host = join(rootfs, f.path);
  if (!existsSync(host)) {
    fail(`${f.path}: not in ${rootfs}`);
    continue;
  }
  const st = statSync(host);
  if (st.size !== f.size) {
    fail(`${f.path}: ${st.size} bytes on disk, manifest says ${f.size}`);
    continue;
  }
  const out = Buffer.alloc(f.size);
  let off = 0;
  let ok = true;
  for (const cid of f.chunks) {
    const raw = chunkBytes(cid);
    if (!raw) {
      ok = false;
      break;
    }
    out.set(raw, off);
    off += raw.length;
  }
  if (!ok || off !== f.size) {
    fail(`${f.path}: chunks do not cover the file`);
    continue;
  }
  if (sha(out) !== sha(readFileSync(host))) fail(`${f.path}: reassembled bytes differ`);
  reassembled++;
  bytes += f.size;
}
console.log(`chunks: ${verified.size} of ${rawLen.size} decoded and hashed `);
console.log(`files: ${reassembled} of ${files.length} reassembled and compared (${(bytes / 2 ** 20).toFixed(1)} MiB)`);
if (sample === 0 && verified.size !== rawLen.size) fail(`${rawLen.size - verified.size} chunks are not in any file`);

if (failures) {
  console.log(`\n${failures} check(s) failed`);
  process.exit(1);
}
console.log('all chunk checks passed');
