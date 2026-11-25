// Synchronous zlib (RFC 1950/1951) decoder, for the chunk store's fallback path (web/chunks.js). The
// engine's reads are synchronous and cannot wait for DecompressionStream, so a chunk that was not in the
// boot pack is decoded here. It follows puff.c (the zlib reference decoder), with a lookup table for the codes of up
// to 10 bits (docs/demo.md, P1: a restore reads its file pages this way). The packs are decoded with
// DecompressionStream instead, which is native.
// Only the chunk store uses it, and its output is checked against the expected length.

const LEN_BASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131,
  163, 195, 227, 258];
const LEN_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
const DIST_BASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049,
  3073, 4097, 6145, 8193, 12289, 16385, 24577];
const DIST_EXTRA = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13];
const CL_ORDER = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];

// A Huffman code as a lookup table indexed by the next `bits` bits of the stream (deflate packs codes from the low bit,
// with the code's bits reversed): each entry is (symbol << 4) | code length, 0 for a code longer than the table. Codes
// up to PRIMARY bits long (nearly every symbol) cost one lookup; a longer one is decoded the way puff.c does, from the
// code's counts and symbols (count, symbol). puff's bit-at-a-time walk for every symbol ran at about 20 MiB/s (6 ms a
// 128 KiB chunk).
const PRIMARY = 10;
function table(lengths, n) {
  let max = 0;
  const count = new Uint16Array(16);
  for (let s = 0; s < n; s++) {
    count[lengths[s]]++;
    if (lengths[s] > max) max = lengths[s];
  }
  const bits = Math.max(1, Math.min(PRIMARY, max));
  const t = new Uint32Array(1 << bits);
  count[0] = 0;
  const offs = new Uint16Array(16);
  for (let len = 1; len < 15; len++) offs[len + 1] = offs[len] + count[len];
  const symbol = new Uint16Array(n);
  for (let s = 0; s < n; s++) if (lengths[s] !== 0) symbol[offs[lengths[s]]++] = s;
  const next = new Uint16Array(16);
  let code = 0;
  for (let len = 1; len <= 15; len++) {
    code = (code + (len > 1 ? count[len - 1] : 0)) << 1;
    next[len] = code;
  }
  for (let s = 0; s < n; s++) {
    const len = lengths[s];
    if (!len) continue;
    let c = next[len]++;
    if (len > bits) continue;
    let r = 0;
    for (let i = 0; i < len; i++) {
      r = (r << 1) | (c & 1);
      c >>= 1;
    }
    const e = (s << 4) | len;
    for (let k = r; k < 1 << bits; k += 1 << len) t[k] = e;
  }
  return { t, bits, mask: (1 << bits) - 1, count, symbol, max };
}

// The stream's bits: up to 31 ahead in buf (refilled a byte at a time), pos the next byte to take.
class Bits {
  constructor(buf, pos) {
    this.buf = buf;
    this.pos = pos;
    this.bitbuf = 0;
    this.bitcnt = 0;
  }
  need(n) {
    while (this.bitcnt < n) {
      if (this.pos >= this.buf.length) {
        if (this.bitcnt >= n) break;
        throw new Error('inflate: input ends early');
      }
      this.bitbuf |= this.buf[this.pos++] << this.bitcnt;
      this.bitcnt += 8;
    }
  }
  bit(n) {
    if (!n) return 0;
    this.need(n);
    const v = this.bitbuf & ((1 << n) - 1);
    this.bitbuf >>>= n;
    this.bitcnt -= n;
    return v;
  }
  // A symbol of the code h. Near the end of the stream fewer than h.bits bits may be left: the code is shorter.
  sym(h) {
    while (this.bitcnt < h.bits && this.pos < this.buf.length) {
      this.bitbuf |= this.buf[this.pos++] << this.bitcnt;
      this.bitcnt += 8;
    }
    const e = h.t[this.bitbuf & h.mask];
    const len = e & 15;
    if (len && len <= this.bitcnt) {
      this.bitbuf >>>= len;
      this.bitcnt -= len;
      return e >>> 4;
    }
    if (h.max <= h.bits) throw new Error('inflate: bad code');
    return this.slow(h);
  }
  // A code longer than the table (puff.c's decode).
  slow(h) {
    let code = 0;
    let first = 0;
    let index = 0;
    for (let len = 1; len <= 15; len++) {
      code |= this.bit(1);
      const count = h.count[len];
      if (code - count < first) return h.symbol[index + (code - first)];
      index += count;
      first += count;
      first <<= 1;
      code <<= 1;
    }
    throw new Error('inflate: bad code');
  }
  // To the next byte boundary, for a stored block: the whole bytes read ahead are given back.
  align() {
    this.pos -= this.bitcnt >> 3;
    this.bitbuf = 0;
    this.bitcnt = 0;
  }
}

// Decodes one block's symbols into out at olen, with the given literal/length and distance codes.
function codes(br, out, olen, lencode, distcode) {
  for (;;) {
    const sym = br.sym(lencode);
    if (sym < 256) {
      if (olen >= out.length) throw new Error('inflate: output too long');
      out[olen++] = sym;
    } else if (sym === 256) {
      return olen;
    } else {
      const li = sym - 257;
      if (li >= 29) throw new Error('inflate: bad length symbol');
      const len = LEN_BASE[li] + br.bit(LEN_EXTRA[li]);
      const di = br.sym(distcode);
      if (di >= 30) throw new Error('inflate: bad distance symbol');
      const dist = DIST_BASE[di] + br.bit(DIST_EXTRA[di]);
      if (dist > olen) throw new Error('inflate: distance too far');
      if (olen + len > out.length) throw new Error('inflate: output too long');
      if (dist >= len) {
        out.copyWithin(olen, olen - dist, olen - dist + len);
        olen += len;
      } else {
        for (let i = 0; i < len; i++, olen++) out[olen] = out[olen - dist];
      }
    }
  }
}

const FIXED_LIT = (() => {
  const l = new Uint8Array(288);
  l.fill(8, 0, 144);
  l.fill(9, 144, 256);
  l.fill(7, 256, 280);
  l.fill(8, 280, 288);
  return table(l, 288);
})();
const FIXED_DIST = table(new Uint8Array(30).fill(5), 30);

function dynamic(br, out, olen) {
  const nlen = br.bit(5) + 257;
  const ndist = br.bit(5) + 1;
  const ncode = br.bit(4) + 4;
  const lengths = new Uint8Array(320);
  for (let i = 0; i < ncode; i++) lengths[CL_ORDER[i]] = br.bit(3);
  const lencode = table(lengths, 19);
  lengths.fill(0);
  let index = 0;
  while (index < nlen + ndist) {
    const sym = br.sym(lencode);
    if (sym < 16) {
      lengths[index++] = sym;
    } else {
      let len = 0;
      let rep;
      if (sym === 16) {
        if (index === 0) throw new Error('inflate: repeat with no length');
        len = lengths[index - 1];
        rep = 3 + br.bit(2);
      } else if (sym === 17) {
        rep = 3 + br.bit(3);
      } else {
        rep = 11 + br.bit(7);
      }
      if (index + rep > nlen + ndist) throw new Error('inflate: too many lengths');
      while (rep--) lengths[index++] = len;
    }
  }
  if (!lengths[256]) throw new Error('inflate: no end-of-block code');
  const lit = table(lengths.subarray(0, nlen), nlen);
  const dist = table(lengths.subarray(nlen, nlen + ndist), ndist);
  return codes(br, out, olen, lit, dist);
}

// Inflates a zlib stream (as zlib.compress and DecompressionStream('deflate') read it) into a Uint8Array of
// exactly rawLen bytes.
export function inflateZlib(input, rawLen) {
  if (input.length < 2 || (input[0] & 0x0f) !== 8 || ((input[0] << 8) | input[1]) % 31 !== 0) {
    throw new Error('inflate: not a zlib stream');
  }
  const out = new Uint8Array(rawLen);
  const br = new Bits(input, 2);
  let olen = 0;
  let last;
  do {
    last = br.bit(1);
    const type = br.bit(2);
    if (type === 0) {
      br.align();
      if (br.pos + 4 > input.length) throw new Error('inflate: input ends early');
      const len = input[br.pos] | (input[br.pos + 1] << 8);
      br.pos += 4;
      if (br.pos + len > input.length || olen + len > out.length) throw new Error('inflate: bad stored block');
      out.set(input.subarray(br.pos, br.pos + len), olen);
      olen += len;
      br.pos += len;
    } else if (type === 1) {
      olen = codes(br, out, olen, FIXED_LIT, FIXED_DIST);
    } else if (type === 2) {
      olen = dynamic(br, out, olen);
    } else {
      throw new Error('inflate: bad block type');
    }
  } while (!last);
  if (olen !== rawLen) throw new Error(`inflate: got ${olen} bytes, want ${rawLen}`);
  return out;
}
