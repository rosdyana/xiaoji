# Input: the browser's taps and keys into Android (I1)

The guest gets two evdev devices, as a phone has them: a multi-touch touchscreen and a keyboard. The engine serves them
(`engine/evdev.c`, `engine/evdev.h`), and the page feeds them through a ring buffer (`web/input.js`) that the engine drains.
EventHub, getevent and the input stack read them with their own ioctls and reads, so nothing in Android changes.

## The devices

| Node | Name (EVIOCGNAME) | Bus, vendor, product, version | What it has |
|---|---|---|---|
| `/dev/input/event0` | `xiaoji_touchscreen` | 0x06 (virtual), 0x0001, 0x0001, 0x0001 | `INPUT_PROP_DIRECT`; EV_KEY `BTN_TOUCH`; EV_ABS `ABS_MT_SLOT` (0..9), `ABS_MT_TRACKING_ID` (0..65535), `ABS_MT_POSITION_X` (0..539), `ABS_MT_POSITION_Y` (0..959), `ABS_MT_PRESSURE` (0..255) |
| `/dev/input/event1` | `xiaoji_keyboard` | 0x06, 0x0001, 0x0002, 0x0001 | EV_KEY: Esc, 1..0, `- =`, Backspace, Tab, Q..P, `[ ]`, Enter, LCtrl, A..L, `; ' \``, LShift, `\`, Z..M, `, . /`, RShift, LAlt, Space, CapsLock, RCtrl, RAlt, Home, arrows, PgUp, PgDn, End, Delete, LMeta, Menu, Back, Homepage. EV_REP (REP_DELAY, REP_PERIOD) |

The ranges come from `engine/display.h` (`XJ_DISPLAY_W` and `XJ_DISPLAY_H`, 540 by 960). `rcGetFBParam` reads the same
two values (`engine/gl_null.c`), so the framebuffer and the touchscreen have one size. Both nodes are character devices,
`13:64` and `13:65`, owner root, group `input` (1004, `engine/ids_gen.h`), mode `0660`, as on a phone. `/dev/input` lists `event0` and `event1` (getdents), so EventHub's first scan finds them
and the inotify watch on the directory succeeds (B9). Nothing is added with IN_CREATE, because the devices exist at startup.

Two configuration files:

- `/vendor/usr/idc/xiaoji_touchscreen.idc` (a synthetic file, its text made at open): `touch.deviceType = touchScreen`,
  `touch.orientationAware = 1`. EventHub finds it by the device's canonical name, and the boot log shows it as the
  device's `configuration=`.
- No keylayout is shipped. Generic.kl maps every key the keyboard has, including `KEY_BACK` (158) and `KEY_HOMEPAGE` (172).
  Esc is sent as Back (158), so `ESCAPE` is never needed.

## The ioctls

Implemented, as the uapi (`linux/input.h`) answers them. The ioctl number is decoded as dir, size, type `'E'`, nr. Length
ioctls return the bytes copied, and the fixed ones return 0, as the kernel does (EventHub checks for zero on
`EVIOCGVERSION`, `EVIOCGID`, `EVIOCGABS`).

| ioctl | Answer |
|---|---|
| `EVIOCGVERSION` | 0x010001, return 0 |
| `EVIOCGID` | bus, vendor, product, version, return 0 |
| `EVIOCGREP` / `EVIOCSREP` | delay 250 and period 33 ms by default; the set is kept |
| `EVIOCGNAME(len)` | the name and its NUL, truncated to `len` (no NUL when cut, as `str_to_user`) |
| `EVIOCGPHYS(len)` | `xiaoji/input0` or `xiaoji/input1` |
| `EVIOCGUNIQ(len)` | `-ENOENT` (no unique id) |
| `EVIOCGPROP(len)` | `INPUT_PROP_DIRECT` for the touchscreen, none for the keyboard |
| `EVIOCGBIT(0, len)` | the types the device has (EV_SYN, EV_KEY, EV_ABS; EV_REP for the keyboard) |
| `EVIOCGBIT(ev, len)` | the codes of type `ev`, in `min(len, longs)` bytes; the bit length is the kernel's `*_MAX` |
| `EVIOCGABS(abs)` | the absinfo (six `__s32`), return 0; `value` is the axis's last value from the events (EventHub's `getAbsoluteAxisValue` reads it); `-EINVAL` for an axis the device lacks |
| `EVIOCGKEY(len)` | the keys held down (kept from the events) |
| `EVIOCGLED`, `EVIOCGSND`, `EVIOCGSW` | all zero |
| `EVIOCGRAB` | one fd holds the device; a second grab is `-EBUSY`; a release by a non-owner is `-EINVAL` |
| `EVIOCSCLOCKID` | `CLOCK_REALTIME` (0, the default), `CLOCK_MONOTONIC` (1) and `CLOCK_BOOTTIME` (7); others `-EINVAL` |

Not implemented (`-ENOTTY`): `EVIOCGKEYCODE`, `EVIOCSKEYCODE`, `EVIOCGMTSLOTS`, `EVIOCSABS`, `EVIOCSFF` (no force feedback),
`EVIOCREVOKE`. None of them is used by getevent or by the remote inputflinger and libinput sources, except `EVIOCSFF`:
EventHub calls it only to vibrate a device that has force feedback, and these devices have none.

## Reads, readiness, queues

- `struct input_event` is 24 bytes on arm64 (timeval as two 8-byte words, then `type`, `code` and `value`). A read returns
  whole events only, up to the last complete packet (a SYN_REPORT); a partial packet stays queued. A buffer under 24 bytes is
  `-EINVAL`; with no complete packet, `-EAGAIN` for a non-blocking fd.
- Each open has its own queue of 256 events, stamped in the clock the fd chose. A full queue drops the event and marks an
  overflow; the next packet is preceded by `SYN_DROPPED` (the client resyncs), as the kernel does.
- `poll` and `epoll` report `POLLIN` once a SYN_REPORT is queued, and `POLLOUT` always.
- A blocked reader waits 2 ms and looks again (`evdev_recheck_interval_ns`). The source may have an event at any time, so
  there is no wake-up from the page; this is the polling cost of the design (see the gaps).
- A write is accepted and ignored (EventHub writes no output events to these keyboards).

## The source of events

- **Native and Node:** `--input-script FILE`, or the environment variable `XIAOJI_INPUT_SCRIPT` (read on the first open).
  One event per line: `ms dev type code value`. `ms` is from the first open of a device, `dev` is 0 (touchscreen) or 1
  (keyboard). `tests/input/evdev.script` is the test's script.
- **Browser (`XIAOJI_WEB`):** the ring, below. `--input-script` is refused there.

Each device gets the events of its own type; a code the device lacks is dropped, as the kernel's input core does. A
device's key state follows its EV_KEY values, and a value of 2 (repeat) leaves it unchanged. An event with no open fd on
its device is taken from the source and dropped, as with the kernel (a client that is not open gets nothing).

## The ring buffer (page to engine)

The engine's worker runs wasm without an event loop, so the page cannot `postMessage` events to it. The events go through
a `SharedArrayBuffer` that both sides map as an `Int32Array`:

| Word | Contents |
|---|---|
| 0 | head: events written (the page's counter, modulo 2^31) |
| 1 | tail: events read (the engine's counter, modulo 2^31) |
| 2 + 4 i .. 5 + 4 i | entry i: `dev`, `type`, `code`, `value` |

The capacity is `(length - 2) / 4` entries (256 here). The page writes the four words, then `Atomics.store`s the head. The
engine loads the head and the tail, copies one entry when they differ, and `Atomics.store`s the tail plus one. A full ring
drops the event (`ringPush` returns false). There is one writer and one reader.

`xiaoji_input_pop` (`engine/jit_lib.js`) is the engine's reader. It is called from `evdev_pump`, which every read, readiness
check and wait makes. The worker gets the buffer in its `boot` message and sets `Module.inputRing` before `callMain`. A page
that is not cross-origin isolated has no `SharedArrayBuffer`, so it gets no ring and no input (the boot still runs).

## The page

- **The screen:** `#android-screen`, the frame canvas of G2 inside `#phone` (`web/input.js` creates an element of its own only
  when the page has none). It takes the pointer (B17: the merged G2 stylesheet had `pointer-events: none` on it, so no tap
  reached it), and once frames arrive the boot screen's decorations are hidden while Stop and Show log stay above it. The
  canvas letterboxes the 540 by 960 picture (`object-fit: contain`), so a pointer is mapped through the picture's box
  (`contentRect`), not the element's.
- **The display size on the page:** `web/input.js` keeps its own 540 by 960 (`DISPLAY_W`, `DISPLAY_H`), because the page does
  not read the engine's header. `tests/input.mjs` fails if it differs from `engine/display.h`.
- **Pointers:** `pointerdown`, `pointermove`, `pointerup` and `pointercancel` on the screen become protocol B
  (`createTouch`): a slot per pointer id (10 slots), a tracking id per contact, `ABS_MT_POSITION_X/Y` from the element's rect
  mapped to 540 by 960 (clamped), `ABS_MT_PRESSURE` from the pointer's pressure (0.5 for a mouse), and `BTN_TOUCH` while any
  contact is down. Each change ends with `SYN_REPORT`. The screen takes the pointer capture and the focus on a press. Only the
  left mouse button counts.
- **Keys:** `keydown` and `keyup` on the focused screen send `KeyboardEvent.code` mapped to the Linux `KEY_` code (for
  example `KeyA` is 30, `Digit1` is 2, `ArrowUp` is 103). Escape is Back (158). Auto-repeat is not sent, because Android
  repeats keys itself. A press with Cmd (meta) or Ctrl is not taken, so the browser's shortcuts keep working. Keys held when
  the screen loses focus are released.
- **Buttons:** Back (`#btn-back`, `KEY_BACK`) and Home (`#btn-home`, `KEY_HOMEPAGE`) sit under the phone and are enabled while
  the boot runs. Each sends one press and release.

## Tests

- `tests/guest/evdev.c` with `tests/expect/evdev.out` (engine and Node, with `tests/input/evdev.script`): the directory and
  the nodes, the ioctls above (EVIOCGABS reports the last value), a stamp check (an event's time lies in the window of the
  read, in the monotonic clock EVIOCSCLOCKID chose), the reads (including EINVAL and EAGAIN), a blocking read on a fresh fd
  (the Back key at 300 ms), the grab results, an inotify watch on `/dev/input`, an epoll wait for the Menu key at 600 ms,
  and a ppoll that times out after 100 ms. The expectation is derived from the uapi and the script (there is no native
  oracle for these devices).
- `tests/init/evdev.rc` with `tests/expect/init-evdev.out`: `getevent -lp` under init as `system:input`, which lists both
  devices (the names, the ranges, the props, the keys and REP). getevent exits after the scan with `-p`.
- `tests/input.mjs` (in `make test-web`): the key map, the display mapping (and the letterboxed box, B17), the frames of a
  two-finger touch, and the ring round trip through the engine's own `xiaoji_input_pop`, loaded from `engine/jit_lib.js`.
- In a browser boot (B17, with `?debug=1`): a tap on the phone, sent after `sys.boot_completed`, was delivered to the
  touchscreen's one open reader (EventHub) with its slot, position, pressure and `BTN_TOUCH` (the engine's
  `xiaoji: evdev:` lines). What Android did with it was not established (docs/boot.md, B17). A tap sent before the boot
  completed was dropped (`InputDispatcher: Dropped event because input dispatch is disabled`, one line in one run).

## Not done

- **Text input.** The keyboard sends key codes only. Android's IME (an input method service, with its own text) is not
  wired, so characters come from Generic.kl's key map and nothing else.
- **Mouse wheel and relative pointers.** No EV_REL device: a wheel on the screen does nothing.
- **Gestures.** The pointers are sent as they are; there is no recognition in the engine (Android recognises them).
- **Output to the devices.** LEDs, sounds and force feedback are answered as absent.
- **The guest reading the ring in a browser.** The ring is checked by `tests/input.mjs` with the engine's own consumer, not in
  a browser run of the guest test.
- **The boot check (`XIAOJI_INPUT_BOOT=1`) has not passed.** It is written (`tests/run.sh`) and runs, but on the Google-image
  tree (`build/rootfs`) the boot cannot reach the input stack. Its log shows `system_server` in the constructor of
  `DisplayManagerService` (`DisplayManagerService.java:501`, the Watchdog's stack) from about 50 s of guest time, waiting for
  SurfaceFlinger, which this tree does not publish (no vndservicemanager, no composer; docs/boot.md B5). Android's own
  Watchdog logs that stack at about 236 s. `InputManagerService` (created in `startOtherServices`, after
  `StartDisplayManager`) never starts, so EventHub logs nothing. The product tree (`build/rootfs-xiaoji`) is what the check is for. It is not in this worktree, and
  building it (from the vendor image, B4d) is outside this change. `dumpsys input` was not reached either.
- **Wake-up.** A blocked reader polls every 2 ms, and nothing wakes it from the page: the page does not `Atomics.notify` the
  engine. The idle scheduler sleeps in `host_sleep_ns` (`nanosleep`, in `engine/host.c`) for up to `IDLE_POLL_NS`, 10 ms
  (`engine/sched.c`); whether emscripten's `nanosleep` in the worker blocks is not checked, so an idle engine may see an event
  up to one sleep later. An `Atomics.wait` on the ring would remove that; it is not built.
