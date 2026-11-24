// The Android screen on the boot page (docs/graphics.md, G2). The engine's worker posts each frame SurfaceFlinger
// composes (rcFBPost, engine/gl_webgl.c): RGBA bytes, top row first, in a transferred ArrayBuffer. This draws it on
// a canvas over the phone frame, and counts it as taken: the worker posts the next frame only after that (the
// shared Int32Array ctl: [0] frames posted, [1] frames taken).

// Distinct colours among a sample of the pixels (at most 65): a uniform frame has one.
export function distinctColors(px, w, h) {
  const seen = new Set();
  const n = w * h;
  const step = Math.max(1, Math.floor(n / 4096));
  for (let i = 0; i < n; i += step) {
    const o = i * 4;
    seen.add((px[o] << 16) | (px[o + 1] << 8) | px[o + 2]);
    if (seen.size > 64) break;
  }
  return seen.size;
}

export function newFrameView(canvas, ctl) {
  const ctx = canvas.getContext('2d');
  const st = { frames: 0, firstMs: null, lastMs: null, lastSeq: 0, w: 0, h: 0, distinct: 0 };
  return {
    state: st,
    show(m, sinceStartMs) {
      if (canvas.width !== m.w || canvas.height !== m.h) {
        canvas.width = m.w;
        canvas.height = m.h;
      }
      const px = new Uint8ClampedArray(m.px);
      ctx.putImageData(new ImageData(px, m.w, m.h), 0, 0);
      canvas.hidden = false;
      st.frames++;
      if (st.firstMs === null) st.firstMs = sinceStartMs;
      st.lastMs = sinceStartMs;
      st.lastSeq = m.seq;
      st.w = m.w;
      st.h = m.h;
      st.distinct = distinctColors(px, m.w, m.h);
      if (ctl) Atomics.add(ctl, 1, 1);
      return st;
    },
  };
}
