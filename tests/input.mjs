// The page's input (web/input.js) and the engine's end of its ring (xiaoji_input_pop in engine/jit_lib.js), without a
// browser (docs/input.md). The producer writes with the page's functions, and the consumer is the engine's own function,
// loaded from engine/jit_lib.js with a stub addToLibrary and the wasm heap as an Int32Array. Exit 1 on any mismatch.
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import {
  createRing, createTouch, displayPoint, keyCode, ringPush, RING_CAP, DISPLAY_W, DISPLAY_H,
} from '../web/input.js';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
let fails = 0;
const check = (name, got, want) => {
  const a = JSON.stringify(got);
  const b = JSON.stringify(want);
  if (a === b) {
    console.log(`PASS  ${name}`);
  } else {
    fails++;
    console.log(`FAIL  ${name}\n        got  ${a}\n        want ${b}`);
  }
};

// Keys: KeyboardEvent.code -> KEY_ (linux/input-event-codes.h).
check('keyCode letters', [keyCode('KeyA'), keyCode('KeyQ'), keyCode('KeyZ'), keyCode('KeyM')], [30, 16, 44, 50]);
check('keyCode digits', [keyCode('Digit1'), keyCode('Digit0')], [2, 11]);
check('keyCode control', [keyCode('Enter'), keyCode('Backspace'), keyCode('Space'), keyCode('Tab')], [28, 14, 57, 15]);
check('keyCode arrows and paging', [keyCode('ArrowUp'), keyCode('ArrowLeft'), keyCode('PageDown')], [103, 105, 109]);
check('keyCode Escape is Back', keyCode('Escape'), 158);
check('keyCode unsent key', keyCode('F13'), -1);

// Points: the element's rect maps to the display, clamped.
const rect = { left: 10, top: 20, width: 360, height: 640 };
check('displayPoint centre', displayPoint(rect, 190, 340), { x: 360, y: 640 });
check('displayPoint clamped', displayPoint(rect, 1000, -50), { x: DISPLAY_W - 1, y: 0 });
check('displayPoint corner', displayPoint(rect, 370, 660), { x: DISPLAY_W - 1, y: DISPLAY_H - 1 });
check('displayPoint zero rect', displayPoint({ left: 0, top: 0, width: 0, height: 0 }, 5, 5), { x: 0, y: 0 });

// Protocol B: the frames a touch sends, as (dev, type, code, value) and SYN_REPORT at each frame's end.
const sent = [];
const touch = createTouch((...e) => sent.push(e));
const frames = (...all) => all;
check('touch down (first contact)', (touch.down(1, 100, 200, 0.5), sent.splice(0)),
  frames([0, 3, 47, 0], [0, 3, 57, 1], [0, 3, 53, 100], [0, 3, 54, 200], [0, 3, 58, 128], [0, 1, 330, 1], [0, 0, 0, 0]));
check('touch down (second contact, no BTN_TOUCH)', (touch.down(2, 300, 400, 1), sent.splice(0)),
  frames([0, 3, 47, 1], [0, 3, 57, 2], [0, 3, 53, 300], [0, 3, 54, 400], [0, 3, 58, 255], [0, 0, 0, 0]));
check('touch move', (touch.move(1, 110, 210, 0.5), sent.splice(0)),
  frames([0, 3, 47, 0], [0, 3, 53, 110], [0, 3, 54, 210], [0, 3, 58, 128], [0, 0, 0, 0]));
check('touch up (one left: BTN_TOUCH stays down)', (touch.up(1), sent.splice(0)),
  frames([0, 3, 47, 0], [0, 3, 57, -1], [0, 0, 0, 0]));
check('touch up (last: BTN_TOUCH up)', (touch.up(2), sent.splice(0)),
  frames([0, 3, 47, 1], [0, 3, 57, -1], [0, 1, 330, 0], [0, 0, 0, 0]));
check('touch up of an untracked pointer', [touch.up(2), touch.move(9, 0, 0, 0), sent.length], [false, false, 0]);
check('touch count after all up', touch.count, 0);

// The ring, written by the page and read by the engine's xiaoji_input_pop.
check('createRing needs cross-origin isolation (absent here)', createRing(), null);
const ring = new Int32Array(new SharedArrayBuffer(4 * (2 + 4 * RING_CAP)));
const heap = new Int32Array(64);
let lib = null;
const addToLibrary = (o) => {
  lib = o;
};
const Module = { inputRing: ring };
new Function('addToLibrary', 'HEAP32', 'Module', readFileSync(join(root, 'engine/jit_lib.js'), 'utf8'))(
  addToLibrary, heap, Module,
);
const pop = () => {
  const r = lib.xiaoji_input_pop(0);
  return r ? [heap[0], heap[1], heap[2], heap[3]] : null;
};

check('ring push', [ringPush(ring, 0, 3, 47, 5), ringPush(ring, 1, 1, 30, 1)], [true, true]);
check('engine pops in order', [pop(), pop(), pop()], [[0, 3, 47, 5], [1, 1, 30, 1], null]);

let fill = 0;
while (ringPush(ring, 0, 0, 0, fill)) fill++;
check('a full ring drops', [fill, ringPush(ring, 0, 0, 0, -1)], [RING_CAP, false]);
check('a full ring reads back in order', [pop()[3], pop()[3]], [0, 1]);
check('one slot frees one push', [ringPush(ring, 0, 0, 0, 77), pop()[3]], [true, 2]);
let drained = 0;
while (pop()) drained++;
check('the rest of a full ring drains', drained, RING_CAP - 2);

// Wrap-around: many events through a small ring, in order.
let bad = 0;
for (let i = 0; i < 5 * RING_CAP + 17; i++) {
  ringPush(ring, 1, 1, 2, i);
  const v = pop();
  if (!v || v[3] !== i) bad++;
}
check('wrap-around keeps order', bad, 0);

if (fails) {
  console.log(`input: ${fails} failed`);
  process.exit(1);
}
console.log('input: all passed');
