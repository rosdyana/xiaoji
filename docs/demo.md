# The demo product and its yardstick (B19)

Goal: a visitor's first minute. Boot Android restores a booted snapshot; then the home screen, the app drawer, Settings
and the other apps should respond like a phone's. Cold boot time does not matter here. This note is the product
(`aosp/device/xiaoji`), what was removed and why, the AOT question, the snapshot point, and the measurement method with
the numbers before and after.

Status: **done, with open issues** (the end of this note). B19 part 2 (the end of this note, and docs/boot.md) ended
the media provider's restart loop: the first Files open after a restore settles in 3.7 s on AC. P1 (below) made the snapshot pre-warmed and the restore
faster (on battery with Low Power Mode: the first Settings open 3.5 s against 23.6 s, press to home 5.0 s against
12.3 s; AC not yet measured). The rest of this paragraph is L1's. The product has Calculator and seven fewer apps that ran,
its demo apps are compiled ahead of time, and the snapshot is taken when the booted phone is idle: after a restore the
engine is busy 17% of the time instead of 70%, nothing crashes or ANRs in the first minute, Home settles in 2.5 s
instead of 3.0 s, and a second start of Settings takes 1.8 s. **The first start of Settings is slower: 15.2 s against
11.2 s on AC.** Its cause is not isolated (AOT code's first run in the engine, or the different snapshot point); it is
the next cost.

## The yardstick: `tools/latency.mjs`

`node tools/latency.mjs [--runs N] [--xj DIR] [--label NAME] [--idle S] [--warm] [--calc] [--json FILE] [--fetch-log FILE]`
(or `--summary A.json,B.json,...`: the table of earlier runs) drives the
page in headless Chromium (Playwright, as `tools/snapshot-web.mjs`). Each run is a new browser (nothing cached) that
presses Boot Android, restores the snapshot, and then:

| Step | What is measured |
|---|---|
| press | Boot Android to the first Android frame on the page |
| idle | the next 30 s untouched: the engine's load reports (below): the share of time the engine was busy, guest instructions per second, processes, threads, the processes that ran the most |
| drawer | a swipe up on the home screen; a swipe that does not open the drawer is counted and tried again |
| Settings | a tap on Settings in the drawer |
| home | the Home button |
| `--warm` | the drawer and Settings once more (Settings' second start) |
| `--calc` | Calculator from the drawer, then 1 + 2 = (`build/web-shots/l1-<label>-calc-result.png`) |

For each action, **response** is the time from the input (the swipe's first touch, the tap's release, the button's
click) to the first frame that differs from the frame before the input in more than 1% of the sampled pixels, and
**settled** is the time from the input's end to the last frame that changed the picture (more than 0.2%) before 5 s
without a change (30 s for an app's start: the splash screen can stay unchanged that long while the app's process
works). Frames are timed where the page receives them (`web/main.js` calls `window.xbHooks.onFrame` with each composed
frame; the tool keeps a 15-bit colour of every 6th pixel of every 6th row), the input where the page sends it, and the
log lines (`START u0`, `Displayed`) where the page receives them. The "+T" of `Displayed` is not used: after a restore it
can count from a launch that began before the snapshot (in the first runs, from the launcher's HOME start, whose
transition had not ended when the snapshot was taken: the before snapshot's Settings showed `+2m16s` for a 15 s start).

`--xj DIR` serves another chunk store, so two trees and their snapshots, frozen in directories, are measured with one
engine build (a snapshot restores only into the build that wrote it). The tool prints the medians and ranges, writes the
runs as JSON (each action's frame timeline included), and saves `build/web-shots/l1-<label>-*.png` of the first run, and
the whole log as `l1-<label>.log`.

The drawer's icon positions depend on the tree's apps (`tools/ui-points.mjs`, shared with `tools/snapshot-web.mjs`):
with Calculator the drawer's first row is Calculator, Files, Settings.

### The engine's load report: `--load-every MS` (`?load=MS`)

Every MS milliseconds of host time, a line on stderr (the page's log):

```
xiaoji: load: 1000 ms, 23.00 M insn (22.9 M/s), busy 100%, 44 processes, 488 threads (20 runnable); top: system_server/1076 39%, ...; threads: system_server:binder:1076_1 29%, ...
```

"busy" is the share of the interval the scheduler loop did not spend in its idle sleeps (`sched.c` counts the time
around each sleep); the instructions are every guest thread's; the top processes and threads are by their own
instructions in the interval. A restore applies the option again (the image holds the run that wrote it). Test:
`snapwait load` (engine and Node, O0 and O2): at least three reports in the format, a quiet one with the 2 processes
and 5 threads of the program and none runnable, the program as the busiest process, and reports after a restore.

In the web build the idle sleep (`nanosleep`, Emscripten's `emscripten_thread_sleep` without pthreads) spins, so the
browser's own CPU meter shows the worker at 100% whether Android is busy or not; "busy" is the engine's view.

## The snapshot point: `--snapshot-quiet PCT:WINDOW:MAX`

The page's snapshot used to be taken at the launcher's HOME start + 60 s. The load report shows what the visitor got:
at that point the boot is not over. The broadcast of BOOT_COMPLETED (an ordered broadcast, one receiver after another)
has not even been posted, its receivers start their processes after the restore (dynsystem, cell broadcast, the
package installer, remote provisioner...), the engine is busy 91 to 94% of the 30 s after the restore (launcher 35%,
contacts provider 22%, system_server 12%); one of the two snapshots made that way held 158 runnable threads.

`--snapshot-quiet PCT:WINDOW:MAX` (engine, `snap.c`) makes the snapshot wait, after the condition and
`--snapshot-after`, for the first WINDOW seconds in which the engine was busy PCT% of the time or less, or MAX seconds,
whichever comes first; the line `xiaoji: snapshot: quiet: busy 9% over the last 10.0 s, ...` says which. The page passes
`?snapquiet=` and `?snapat=log:TEXT` (another condition) for `?snapshot=make`. Test: `snapwait snapshot quiet` (engine and
Node, O0 and O2): the condition at 0.05 s and a 0.2 s window at 50% fall in the program's 600 ms sleep; the snapshot says
"quiet", and the restored run's output follows the oracle's.

`make snapshot` (`tools/snapshot-web.mjs make`) now takes the demo's snapshot at the launcher's HOME start + 480 s
(`XN_SNAP_AFTER`), then the first 10 s in which the engine was busy 15% of the time or less, at most 360 s more
(`XN_SNAP_QUIET=15:10:360`). 480 s, because the BOOT_COMPLETED receivers run in that time, and one of them, the remote
provisioner's, waits for a network until the broadcast's timeout ends it with an ANR about 340 s after HOME (open issue
4). The first version of the new point, HOME + 240 s, is measured too ("after, HOME + 240"); the ANR then came 44 s after
every restore. In the final make the quiet window came at once (busy 4%), and the snapshot held 43 processes with no
thread runnable (the old point: 45 processes, 5 and 158 runnable in two makes).

**Pre-warm** (on by default since P1; `XN_SNAP_PREWARM=off`): before the snapshot, Files, Calculator and Settings are each
opened from the drawer with the page's input, as a visitor would, until their START line (and `Displayed` or 60 s), a
screenshot (`build/web-shots/s1-prewarm-<app>.png`), and Home; the condition is the last app's START line + 120 s and
the quiet window. The idea: a visitor's first tap on Settings would be a second start, which takes about 2 s here instead
of 10 to 16 s (the measurements below). It works up to the snapshot (all three started and displayed, quiet at 2% busy),
but the snapshot does not restore in headless Chromium: 997 MiB of memory (765 before), 177 MB of WebGL objects (81
before: the stopped apps' windows and the task snapshots of recents), a 155 MiB download, and the page closed before its
first frame (`tools/snapshot-web.mjs restore`: "Target page, context or browser has been closed"). Making the stopped
apps release their GL buffers (a memory trim before the snapshot) is the open part. `make snapshot` also checks that the
last system_server started SystemUI once (the first boot of that make restarted system_server once, a start-order race:
`SafetyCenterManager` was null in AccessibilityManagerService's boot phase 600).

The Makefile's comment on `snapshot` still says HOME + 60 s: the Makefile is part of the engine's build id (a snapshot
restores only into the build that wrote it), so it was left alone in this change; `tools/snapshot-web.mjs` has the
procedure.

## The product

The visitor's apps: the launcher (Launcher3QuickStep), SystemUI, Settings, Files (DocumentsUI) and **Calculator** (AOSP's
ExactCalculator, Apache-2.0). Calculator left the AOSP manifest after Android 9, so the 13 tree has no copy:
`tools/remote-build.sh` clones its last platform release (`pie-platform-release`, commit `076857e7`, pinned) into
`device/xiaoji/apps/ExactCalculator/upstream` on the build machine, without its `Android.mk` and `CleanSpec.mk`; the
build rule is ours (`aosp/device/xiaoji/apps/ExactCalculator/Android.bp`: SDK "current", the androidx libraries and
`external/crcalc` of the 13 tree, target SDK 30 because the 2019 manifest has no `android:exported`, `/system_ext/app`
because `xiaoji_system.mk`'s artifact path rule keeps product-level modules out of `/system`). Its sources are not in
this repository (Android components are fetched or built, never redistributed).

Removed (each one ran a process on this product; the list is from the engine's thread dump of the native snapshot,
45 processes and 526 threads, and from the processes started after a restore):

| Package | Process | Why it goes | How |
|---|---|---|---|
| SecureElement | `com.android.se` | NFC and the secure element are off | override (`base_system.mk` adds it) |
| LocalTransport | `com.android.localtransport` | the backup transport; backup is "never" (features.md) | override (`base_system.mk`) |
| DynamicSystemInstallationService | `com.android.dynsystem`, `:dynsystem` | started by BOOT_COMPLETED; installs system images | override (`base_system.mk`) |
| MultiDisplayProvider | `com.android.emulator.multidisplay` | the emulator's multi-display; a tab has one display | dropped from `xiaoji_vendor.mk`, and override (`emulator_system.mk` lists it too) |
| SdkSetup | `com.android.sdksetup` | the emulator's setup wizard; Provision provisions the device (it stays, B16), SdkSetup only added emulator settings (auto-rotate, ADB) | dropped from `xiaoji_vendor.mk` |
| EmulatorRadioConfig | - | no telephony | dropped from `xiaoji_vendor.mk` |
| CalendarProvider | `com.android.providers.calendar` | started by an upgrade broadcast; Calendar is an add-on | dropped from `xiaoji_handheld_system.mk` |
| the cell broadcast APEX | `com.android.cellbroadcastreceiver.module` | its makefile (`telephony_system.mk`) was dropped in B16, but `tools/fetch-remote-rootfs.sh` kept the old copy in `/apex` | the fetch now removes `/apex/<name>` directories that `/system/apex` no longer has |

How a package of an inherited makefile is removed: `PRODUCT_PACKAGES` cannot lose a word (B16), but an installed
module can override others, and the build then installs none of them (`build/make/core/main.mk`, the mechanism that
lets Launcher3QuickStep replace Launcher3). `aosp/device/xiaoji/lean/Android.mk` is that module (`xiaoji_lean`, one text
file in `/system_ext/etc` that lists what is left out). RemoteProvisioner (`com.android.remoteprovisioner`, started by
BOOT_COMPLETED) was tried too and stayed: the keymint HAL lists it as a required module, and the override did not take
it out; it is an open issue below.

Kept, with the reason:

- **The media provider** (`com.android.providers.media.module`). It is not idle weight: DocumentsUI asks for its
  `media.documents` roots, and Files, downloads and SystemUI's screenshots use it (its `framework-mediaprovider.jar` is
  on the boot class path, so it cannot be removed without the APEX). Until B19 part 2 (docs/boot.md) it waited for the
  hardware codec2 store (`android.hardware.media.c2@1.1::IComponentStore/default`, declared in the goldfish vendor
  manifest and never started), was killed after 40 s ("timeout publishing content providers"), and started again at
  the next request. Now the device manifest has no hardware store, the software store (`media.swcodec`, with its
  seccomp filter, which the engine enforces) runs, and the provider starts once and stays up.
- Provision (B16: it provisions the device and disables itself), PackageInstaller (PackageManager requires an
  installer), the providers the apps use (contacts in `android.process.acore`, downloads and MTP in
  `android.process.media`, external storage), KeyChain, Telecom (B16), NetworkStack (ConnectivityService needs it).

Settings:

- **`pm.dexopt.disable_bg_dexopt=true`**: no background dexopt job (BackgroundDexOptService) on the engine's one core.
  At the first boot PackageManager still runs dex2oat with "verify" on the APEX apps that have no odex (permission
  controller, ext services, media provider...), before the snapshot.
- **`ro.config.low_ram=true` stays as it was** (set since the first product, `xiaoji_arm64.mk`): it was not A/B-tested in
  this round (a second tree and snapshot); the page's `?prop=K=V` (`--prop` for a boot) is there for that run, which
  needs a snapshot made with `XN_SNAP_QUERY='&prop=ro.config.low_ram=false'`.
- **Animation scales**: not changed, and not measured (open issue 5). The window and transition scales have
  SettingsProvider defaults (an overlay could set them), the animator scale (the drawer's and the app launch's
  animations) has none, and the engine's init has no property triggers to run `cmd settings put` at boot.

## AOT

Before: the launcher, SystemUI, Settings and DocumentsUI were preopted with the **"verify"** filter: no compiled code
in their odex, so ART interprets them and its JIT compiles the hot methods at run time, on the engine's one core. Their
makefiles give no profile, and with no profile and no default filter dexpreopt picks "quicken", which is "verify"
since Android 12 (`build/soong/dexpreopt/dexpreopt.go`). system_server's `services.jar` and the APEX system server jars
were already **"speed"** (dexpreopt's default for system server jars), as was SettingsProvider (a system server app),
and the boot image is "speed-profile".

Now `PRODUCT_DEXPREOPT_SPEED_APPS` compiles the five demo apps in full at build time. The compiler filter, read from
each odex's header (`compiler-filter` in its key-value store; `strings` shows it):

| odex | before | after |
|---|---|---|
| Launcher3QuickStep | verify, 95 KB | **speed, 23.7 MB** |
| SystemUI | verify, 132 KB | **speed, 29.4 MB** |
| Settings | verify, 99 KB | **speed, 24.9 MB** |
| DocumentsUI | verify, 42 KB | **speed, 7.6 MB** |
| ExactCalculator | - | **speed, 1.3 MB** |
| services (system_server) | speed, 46.1 MB | speed, 46.1 MB |
| SettingsProvider | speed, 552 KB | speed, 552 KB |

The tree grows by 83 MB (1,283 to 1,365 MB; 693 new chunks, 37 MB compressed), fetched only where a page is touched.
The snapshot does not grow with it: clean file pages, the odex's among them, are dropped from the image (docs/snapshot.md).

At run time ART uses them: at the first boot PackageManager dexopts every app whose odex it does not accept
("Running dexopt (dexoptNeeded=1)"); in the after tree that list is only APEX apps without an odex (permission
controller, ext services, media provider, ...), none of the demo apps. And the compiled code runs: the chunk store's
requests during one restored run (`tools/latency.mjs --fetch-log`, the shipped snapshot) show the pages of the odex
files fetched when their code first runs. From the tap on Settings (47 s after the press, when `Settings.apk` is
first read) to the end of the run, 144 chunks of `Settings.odex` (128 KiB each, about 18 MB of its 25 MB) were read,
and 3 of `Settings.vdex`; SystemUI read 151 chunks of `SystemUI.odex`, system_server 273 of `services.odex`. (Thread
dumps, `?dumpat=`, also show return addresses in `services.odex`; they never caught Settings' main thread outside the
framework's boot image code, because the frame-pointer chains stop at most ART frames.)

Those reads are a cost of their own: after a restore the tree's chunks are read on demand and synchronously, so a first
start of Settings waits for some 150 chunk reads. On localhost each is a fetch and an inflate; over a network each would
be a round trip (docs/snapshot.md, Open). With "verify" apps the same start reads the dex in the APK instead, a few MB.

What AOT changed is not isolated yet. The instructions to `sys.boot_completed` are about equal (30.2 G before, 30.7 G
after), and a first start of Settings ran about 0.5 G instructions in both trees. A third tree, the demo product with
"verify" apps (built and snapshotted for that comparison), cannot be measured as it is: its SystemUI crash-looped (181
starts before the snapshot: `KeyguardUpdateMonitor` got no `BiometricManager`, "No service published for: auth", a
start-order race that the AOT trees never hit), so its numbers are not used here. Open issue below.

## Measurements

All runs: headless Chromium (Playwright's headless shell, SwiftShader WebGL2), localhost, the M2 (8 GB), one engine build
(`web-3b9ca69e63a16d52`) for every tree: the trees and their snapshots were frozen in directories (`build/demo/*-xj`)
and served with `--xj`. **Before** is the product before this change with its snapshot made the old way (HOME + 60 s);
**after, HOME + 240** is the demo product with the first version of the new point; **after, HOME + 480** is the demo
product with `make snapshot` as it is now (the shipped snapshot). A fresh browser profile per run; medians and ranges;
response / settled as defined in the yardstick section; times in seconds. `node tools/latency.mjs --summary
A.json,B.json,...` prints each column from the runs' JSON (`build/demo/`).

**On AC power, Low Power Mode off** (swap in use 4.3 to 7.2 GB; before and after alternated):

| | before (4 runs) | after, HOME + 240 (3) | **after, HOME + 480 (3)** |
|---|---|---|---|
| press Boot Android to the first frame | 5.8 (5.1 to 9.0) | 5.9 (5.8 to 6.1) | 6.2 (5.9 to 6.4) |
| snapshot download | 107.3 MiB | 122.3 MiB | 115.8 MiB |
| idle 30 s after the restore: engine busy | 70% (63 to 81) | 12% (10 to 14) | **17% (12 to 18)** |
| idle 30 s: guest instructions per second | 28.1 M (21.7 to 29.5) | 1.0 M | **4.8 M (4.7 to 4.8)** |
| processes / threads | 47 / 509 (46 / 491) | 43 / 465 | 44 / 479 |
| swipe up to the drawer: response / settled | 0.4 (0.2 to 0.6) / 1.0 (0.8 to 1.9) | 0.8 (0.4 to 0.9) / 1.4 (1.1 to 1.6) | 1.0 (0.6 to 1.4) / 1.6 (1.5 to 2.1) |
| first Settings start: response / settled | 4.2 (3.6 to 5.6) / **11.2 (9.5 to 12.2)** | 7.4 (4.3 to 12.2) / 15.8 (10.3 to 22.6) | 6.3 (4.4 to 7.3) / **15.2 (10.1 to 16.1)** |
| first Settings start: its START / `Displayed` line in the log | 0.2 (0.2 to 0.3) / 10.0 (8.5 to 11.0) | 0.7 (0.3 to 0.8) / 11.9 (9.2 to 14.6) | 0.3 (0.3 to 1.8) / 13.2 (9.0 to 14.9) |
| second Settings start: response / settled | 1.5 (1.4 to 2.4) / 2.0 (1.6 to 2.9) | 1.6 (1.6 to 1.7) / 2.1 (2.0 to 2.2) | 1.3 (1.2 to 1.6) / **1.8 (1.6 to 2.4)** |
| Home from Settings: response / settled | 2.5 (1.9 to 2.9) / 3.0 (2.6 to 3.5) | 1.8 (1.6 to 2.3) / 2.2 (2.1 to 2.9) | 2.1 (1.8 to 2.3) / **2.5 (2.1 to 2.9)** |
| first Calculator start (a new process): response / settled | - | 2.3 (1.7 to 3.9) / 6.1 (5.3 to 12.1) | 2.0 (1.6 to 2.2) / 6.1 (5.1 to 6.4) |
| Calculator key: response | - | 0.3 (0.2 to 0.6) | 0.3 (0.2 to 0.6) |
| failures in the log after the restore | contacts provider FATAL EXCEPTION, every run | remote provisioner ANR 44 s after every restore | **none** |
| missed inputs (swipes read as a long press / taps that started nothing) | 1 / 0 | 2 / 0 | 1 / 0 |

**On battery, Low Power Mode on** (the first set, swapping; before, then after, HOME + 240):

| | before (3 runs) | after, HOME + 240 (3) |
|---|---|---|
| press Boot Android to the first frame | 8.1 (7.8 to 9.1) | 9.3 (8.8 to 15.8) |
| idle 30 s: engine busy | 93% (91 to 94) | 23% (16 to 27) |
| idle 30 s: guest instructions per second | 26.7 M (22.1 to 27.4) | 0.9 M (0.9 to 1.0) |
| swipe up to the drawer: response / settled | 0.8 (0.7 to 0.8) / 2.0 (1.9 to 2.1) | 1.0 (0.4 to 1.6) / 1.8 (1.0 to 2.5) |
| first Settings start: response / settled | 7.3 (7.2 to 7.4) / 18.4 (17.7 to 21.5) | 11.2 (9.3 to 13.1) / 29.1 (22.6 to 30.5) |
| first Settings start: `Displayed` line in the log | 16.6 (15.9 to 18.5) | - (none logged) |
| second Settings start: response / settled | 2.0 / 2.5 (2.5 to 2.6) | 2.3 (2.2 to 2.4) / 3.0 (2.9 to 3.1) |
| Home from Settings: response / settled | 3.6 (3.4 to 3.9) / 4.4 (4.2 to 4.6) | 6.3 (3.0 to 9.6) / 7.3 (3.6 to 11.0) |
| first Calculator start: response / settled | - | 2.7 (2.6 to 2.8) / 9.1 (8.6 to 9.6) |

The power state matters as much as the product: the same first Settings start (before) takes 18 s on battery with Low
Power Mode and 11 s on AC.

What the numbers say:

- **The restored phone is idle, and clean.** Before, the engine was busy 70 to 93% of the 30 s after the restore (the
  launcher loading, the contacts provider upgrading its database and crashing, BOOT_COMPLETED receivers starting
  processes); now 12 to 18%. What is left (4.8 M instructions per second with the shipped snapshot) is system_server's
  main thread, its `AdbDebuggingManager` and `SensorService` threads, servicemanager and a HAL: periodic polling, open
  issue 7. No ANR, crash or process start lands in the first minute any more.
- **Home is faster** (2.5 s settled, from 3.0) and a **second start of Settings takes 1.8 s**: a warm start is fast enough
  to feel real.
- **The first start of Settings is slower: 15.2 s against 11.2 s.** Settings' process is alive in every snapshot (no
  `Start proc` for it): the cost is its activity's first start. In run 3 of the shipped snapshot (10.1 s) the load
  reports from the START line to the settled frame add up to 501 M guest instructions, against 555 M in a before run of
  9.5 s: fewer instructions, run at 52 M per busy second instead of 67 M (one pair of runs). The cause is not isolated.
  One candidate is the AOT code: a first start runs compiled code the wasm JIT has not translated yet (a restore drops
  every translated block), where the interpreted app ran in the interpreter's few, already translated blocks. The other
  difference is the snapshot point (HOME + 60 s against HOME + 480 s: Settings' process had been cached much longer).
  The controls, in order: the before tree snapshotted at the new point (`XN_XJ=<a copy of before-xj> make snapshot`,
  about 30 min), and an engine counter of the JIT's translation work (open issue 1).
- **Calculator**, a new process each time (`Start proc ... com.android.calculator2`), shows its keypad in 2 s and is
  settled in 6 s; a key answers in 0.3 s. `build/web-shots/l1-final-calc-result.png`: 1 + 2 = 3.
- The snapshot is 8.5 MiB larger (115.8 MiB): about 0.4 s more of download on localhost.

Screenshots (`build/web-shots/`, not committed, the last run of each): `l1-before-{home,drawer,settings,home-after}.png`;
`l1-final-{home,drawer,settings,home-after,calculator,calc-result}.png`; Files, from the pre-warm trial:
`s1-prewarm-files.png` (Downloads, with the media provider's Images, Audio and Videos roots listed). The runs' JSON is in
`build/demo/` (`lat-*.json`, `ac-*.json`).

## P1: the first open of an app after a restore

Goal (P1): after Boot Android, the first open of Settings settled in 3 s or less, Calculator 2 s, the drawer 1 s,
without making press to home worse than about 8 s. Before this round: Settings 11.8 s, Calculator 6.4 s, the drawer
1.9 s (the table below).

Status: **not verified against the target.** The pre-warmed snapshot works, after a defect that corrupted every
image above 2 GB was found and fixed (item 7 below). The final engine and snapshot were measured only after the Mac
had gone onto battery with Low Power Mode on (not something this round could change): there, against the L1
snapshot measured in the same conditions, interleaved, the first Settings open settles in **3.5 s against 23.6 s**,
Calculator in 4.1 s against 12.8 s, press to home 5.0 s against 12.3 s, and the idle browser uses 16% of a core
against 100%. The drawer is not faster (2.4 s both). L1's battery numbers ran about 1.6 to 2 times its AC numbers,
so on AC the targets for Settings (3 s) look within reach and Calculator's (2 s) and the drawer's (1 s) do not;
the AC runs are the first thing to do next (`build/p1/interleave2.sh`).

### What changed, and why

1. **The demo apps are warm in the snapshot (the pre-warm, now on by default).** `make snapshot` opens Files,
   Calculator and Settings from the drawer, waits for each one's `Displayed` line (150 s; a start that is not drawn is
   tried again), sends the phone home and takes the snapshot when the engine is quiet (`tools/snapshot-web.mjs`,
   `XN_SNAP_PREWARM=off` for the old point). A visitor's first open is then a hot start: the activity exists, and
   the launcher's tap brings its task to the front (`START` with no `Start proc`, no `Displayed` line).
   - *Why the L1 pre-warm did not restore:* not isolated. Two candidates, both fixed: the 2 GB defect (item 7; L1's
     pre-warmed image was 997 MiB, which puts its top above 2 GB), and the restore's memory. `web/worker.js` zeroed
     the image's whole range (about 2 GB: every page of the new wasm memory written, although new memory is zero)
     and inflated every chunk of every pack at once (about 1 GB of raw chunks in flight), on a machine with 4 to 8 GB
     of swap in use. Now only the memory the module had before the grow is zeroed, at most 3 packs download at once,
     and four helper workers (`web/inflater.js`) inflate the chunks in batches of 32, each written into memory as it
     arrives (`?inflaters=N`, 0 inflates in the engine's worker). The make's browser peaked at 1.2 to 1.4 GB and the
     restores at about 0.9 GB; none crashed. `tools/snapshot-web.mjs` reports a crash of the page or the browser now,
     with the browser's resident sets, and prints the image's top.
   - *The cached-app freezer is off.* The engine answered `BINDER_GET_FROZEN_INFO` (no process frozen) but not
     `BINDER_FREEZE`, so ActivityManager believed it could freeze, failed, and killed every cached app about 10 minutes
     after it was cached ("Unable to freeze binder interface"): a pre-warmed app would have died in the visitor's
     first minutes. The engine now answers both with EINVAL, as a kernel without the freezer does, and the framework
     turns the freezer off (`unable to read freezer info`, `Freezer disabled` in every make since). Test:
     `binder-freeze` (expectation case, engine and Node). After the restores, the only kills in the logs are the media
     provider's known loop (open issue 3) and `android.process.media`'s background ANR, both before this round too;
     no demo app was killed (`Killing` lines are in each run's JSON, `build/p1/`). `D/OomAdjuster: Not killing cached
     processes` is the low-RAM device's policy (lmkd kills; it has no pressure here).
   - *Size.* The snapshot grew from 115.8 MiB to 141.8 MiB (916 MiB of memory against 765). The WebGL objects grew
     most: 177 MB, of which 66 textures of 1 MiB or more make 166 MiB (four 2048x1024 textures of 8 MiB and many
     922x1024 ones of 3.6 MiB: the cached apps' window buffers and the task snapshots of recents; the census line
     `textures of 1 MiB or more` names them). Dropping those (a memory trim before the snapshot) is open.
2. **The restore no longer waits on the network for the file pages a visitor touches first: the prefetch.** The
   snapshot drops clean file pages (code of libraries, the boot image, the apps' odex); after a restore each first
   touch read its chunk with a synchronous request and a JS inflate. In the first warm run the drawer made 180 such
   reads (505 ms fetch + 214 ms inflate) and Settings 296 (466 + 297 ms). `tools/prefetch.py record` turns a run's
   server log (`tools/latency.mjs --fetch-log`) into the order of first reads (`image/restore-order.json`, as
   [path, chunk] pairs that survive a rebuilt tree: 1,372 chunks of a session with the drawer, Settings, Calculator
   and Files), and `tools/prefetch.py pack` packs them into 33 packs of 2 MiB (64.5 MiB, 159 MiB raw) listed in
   `snapshot.json` as `prefetch` (`make snapshot` runs it). The engine's worker starts `web/prefetch.js` before the
   engine runs; it fetches and inflates the packs (native `DecompressionStream`) into a SharedArrayBuffer, and the
   chunk store takes a chunk from there when its pack is in, waits up to 250 ms for a pack in flight, and otherwise
   reads it itself (`?prefetch=0` turns it off). Result: the drawer's and Settings' chunk reads went to 0 or 1.
   - *Item 2 of the task (demand paging of the snapshot).* Not done: the restore is still eager. It did not need to
     be lazy this round: with the loader above, a 142 MiB snapshot restores to its first frame in 2.6 s on AC
     (median of 3) against 6.4 s for 116 MiB before, and in 5.0 s against 12.3 s on battery. Packs are in snappack's first-use order (the address order of the image), not first touch:
     with an eager restore the order does not change when the engine starts.
3. **A faster synchronous inflater** (`web/inflate.js`): a 10-bit lookup table for the Huffman codes instead of
   puff's bit-at-a-time walk, 20 to 91 MiB/s (6 ms to 1.4 ms a 128 KiB chunk). Every chunk not prefetched still goes
   through it. Test: `tests/snapshot.mjs` (stored, fixed, dynamic blocks, overlapping copies, long codes, a broken
   stream, and every 8th chunk of the tree's store against Node's zlib; `XIAOJI_INFLATE_ALL=1`: all 11,339).
4. **The JIT's pre-hot set** (`--jit-prehot`, on for a restore; `?prehot=0` off). A restore drops every translated
   block, and each block then ran 1,000 times in the interpreter before it was queued again. At the snapshot the
   engine now records the guest pcs of its translated blocks (before the clean-page drop invalidates the blocks on
   file pages), and after the restore a pc of that set is queued at its first run. Test: `jitstress jit pre-hot node`.
   The `load+` lines show translation is not the cost now: 90 to 190 ms of translating in a Settings open.
5. **The idle engine sleeps** (`engine/host.c`): Emscripten's `nanosleep` spun without pthreads; `host_sleep_ns` now
   waits in `Atomics.wait` on the input ring's head (the page notifies it with each event, `web/input.js`), and on a
   private word in Node. The browser's CPU in the 25 s after the restore's first 5 s: **100% before, 9 to 12% after**
   (all of the browser's processes, `ps`; one core = 100%; 16% against 100% on battery). Input is not slower:
   Calculator's key response is the same before and after (0.3 s on AC, 0.5 s on battery). Test: `init-idle-sleep idle cpu` (engine and Node): host CPU under half of the wall time of a
   run that is mostly sleeps (Node: 1.9 s of CPU in 8.7 s).
6. **Measurement.** The engine's load report has a second line, `xiaoji: load+:`, with the share of instructions in
   translated code, the blocks translated and the host time it took, the pre-hot set, and (in the page) the chunk
   store's reads, their fetch and inflate time, and the prefetched chunks. `tools/latency.mjs` keeps them per action,
   measures the browser's idle CPU, takes `--wasm DIR` (a frozen engine with its snapshot), and `--files` (Files'
   first open). The snapshot census adds the largest textures and the largest processes.

7. **A snapshot above 2 GB was corrupt** (`engine/snap.c`, `engine/gl_snap.js`, `engine/jit_lib.js`, `engine/sched.c`).
   A C pointer reaches JavaScript as a signed 32-bit number, so a pointer at or above 2 GB arrives negative.
   `xj_snap_post` sent `HEAPU8.slice(p, p + len)`, and every extent of the image above 2 GB was posted from the wrong
   place. The pre-warmed images end above it (0x82ae7000 and 0x840f0000, against 0x72fac000 before): in the first
   two pre-warmed snapshots the first extent above 2 GB held the engine's own format strings. Those snapshots
   restored, showed their first frame (a restored color buffer) and drew blank screens at the first redraw by an app
   (the status bar's clock, about 27 s later): no text, no icons in Settings, a home screen with the wallpaper only.
   The pointers of every JavaScript function the engine calls with one (the post, the GL blob's save and load, the
   JIT's module and slots, the input ring, the load report's text) are now taken with `>>> 0`. The fixed snapshot ends
   at 0x8155a000 and draws (`build/web-shots/p1-*.png`). `make test-snapshot-web` checks it now: Settings' frame after
   the restore must have 20 colours or more (64 in the fixed run; the blank one had 2). This is probably also why L1's
   pre-warmed snapshot did not restore (its image was 997 MiB), but that was not tested; the item 1 notes on memory
   are the other candidate.

### Measurements

Headless Chromium on the M2 (8 GB), localhost; `tools/latency.mjs --warm --calc --files`, a new browser per run;
medians (range) in seconds; response / settled as in the yardstick section. **Before** is the shipped L1 snapshot with
its engine (`build/p1/before-wasm`, `build/demo/final-xj`; the page's new loader, prefetch and pre-hot set off).
**After** is the final engine and its pre-warmed snapshot (`make snapshot` of these sources, frozen in
`build/p1/fixed-*`). Before and after alternated.

**Battery, Low Power Mode on** (the final comparison; swap in use 5.4 to 5.9 GB):

| | before (2 runs) | **after (3 runs)** | target (AC) |
|---|---|---|---|
| press Boot Android to the first frame | 12.3 (9.7 to 14.9) | **5.0 (4.9 to 7.0)** | about 8 |
| snapshot download | 115.8 MiB | 141.8 MiB (+ 64.5 MiB of prefetch in the background) | |
| idle: browser host CPU (one core = 100%) | 100% (99 to 101) | **16% (13 to 18)** | |
| idle: engine busy / guest instructions | 24% / 4.8 M/s | 22% / 2.6 M/s | |
| drawer: response / settled | 1.1 / 2.4 (2.1 to 2.7) | 1.2 (1.0 to 1.8) / 2.4 (1.9 to 4.0) | 1 |
| first Settings open: response / settled | 9.2 (9.0 to 9.5) / 23.6 (23.3 to 23.9) | 3.3 (2.9 to 4.9) / **3.5 (3.2 to 5.3)** | 3 |
| first Settings open: guest instructions | 560 M | 66 to 87 M | |
| second Settings open: response / settled | 2.1 / 2.8 | 2.6 / 3.1 | |
| Home from Settings: response / settled | 3.6 / 4.1 | 3.9 / 4.4 | |
| first Calculator open: response / settled | 3.7 / 12.8 (8.7 to 16.8) | 3.4 (2.1 to 7.6) / **4.1 (2.6 to 8.0)** | 2 |
| Calculator key: response | 0.5 | 0.5 | |
| first Files open: response / settled | 9.7 / 42.3 (1 run) | 6.7 (1 run) / 17.1 (2.5 to 31.7) | |

**AC power, Low Power Mode off** (swap 4.4 to 5.9 GB): only before was measured on a correct snapshot (5 runs: 3 at
the start, 2 interleaved later): press 6.4 (4.8 to 7.4), drawer 1.0 / 1.7, first Settings 5.3 / 11.8 (10.2 to 14.6),
second Settings 1.3 / 1.8, Home 2.0 / 2.4, Calculator 2.4 / 6.5, the idle browser 100% of a core. The after runs on AC
(`build/p1/final-*.json`, `warm1*.json`) were of the corrupt snapshots (item 7): they measured screens without text
and are not used. One of their numbers holds, because it does not depend on what is drawn: press to the first frame
2.6 s (2.6 to 6.0) for a 143 MiB snapshot.

An experiment that is not in the shipped snapshot: the window, transition and animator scales at 0.5 (an init service
that ran `settings put global` after `sys.boot_completed`). That snapshot stayed under 2 GB and drew correctly; the
drawer settled in 1.0 s and Calculator in 2.3 s (AC, 3 runs), against 1.3 s and 3.3 s for a snapshot without it made
the same day (but corrupt, so not a clean comparison). In both makes with it the pre-warm's Settings was not drawn
(one took the snapshot without Settings' `Displayed`, so the visitor's first open was a cold 10 s start; the next
gave up after three tries), so it was taken out. Whether the scales caused that is not known.

Screenshots: `build/web-shots/p1-{home,drawer,settings,home-after,calculator,calc-result,files}.png` (the last after
run; Calculator shows 1 + 2 = 3). The runs' JSON: `build/p1/`.

### Where the time goes now (a Settings open, after, battery)

From the tap's release (the 3 after runs; the engine's load reports and the frame timeline):

| Phase | Time | What |
|---|---|---|
| input to `START` | 0.1 s | the launcher's tap handling and its binder call to ActivityTaskManager |
| `START` to the first changed frame | 2.8 to 4.8 s | the task brought to the front: the launcher's opening animation, Settings' `onRestart`, `onStart`, `onResume` (its `DashboardFragment.onResume` refreshes every top-level summary over binder) and its first draw; SurfaceFlinger composes |
| to the settled frame | 0.2 to 0.4 s | the last frames of the animation |
| in all | 66 to 87 M guest instructions | system_server 37 to 39%, the launcher 19 to 25%, SurfaceFlinger 20 to 22%, Settings 9 to 13% |

Not the cost any more: file reads (0 or 1 chunk: the prefetch), translation (88 to 190 ms of host time), GC (not in the
top threads). The first Settings open is now a hot start of 66 to 87 M instructions, against 560 M for L1's cold
activity start in a warm process. The engine is busy 83 to 99% during it (the load reports): it is not waiting on
frames, it runs out of speed, at about 20 to 25 M instructions a second with Low Power Mode on (L1's AC runs: 50 to 70
M). **The next-biggest cost is the engine's speed on the transition's work**: system_server's window animation and
binder calls (37 to 42%), the launcher (19 to 25%) and SurfaceFlinger (20 to 22%) do most of it, Settings itself 9 to
13%. In the drawer's first open, `trigger_perfetto` (a process the launcher starts for its jank trace triggers) ran 27
to 30% of the instructions in two of three runs: removing it from the product is a cheap next step. The other levers:
fewer animation frames (the scales above, once the pre-warm interaction is understood) and the engine's speed (50 to
70% of the instructions ran translated in these actions).

Open, from these runs: the second Settings open is slower after than before (3.1 s against 2.8 s on battery; 2.7 s
against 1.8 s in the AC runs of the corrupt snapshots), and Home from Settings slightly so (4.4 s against 4.1 s). Not
isolated: the idle wait, the prefetch worker and the pre-warmed apps' extra live processes are all candidates. Files
is pre-warmed but not warm in effect: in one run of three its process was started again on the tap (`Start proc
com.android.documentsui`), and it waits on the media provider's restart loop either way (17 s settled, 2.5 to 31.7;
open issue 3).

### P1 verification (final sources)

| Command | Result |
|---|---|
| `make && make wasm-node && make wasm` | built, no warnings |
| `XIAOJI_JOBS=4 tests/run.sh` | **311 passed, 0 failed, 0 xfail**, with the new `binder-freeze` (engine, Node), `init-idle-sleep idle cpu` (engine 1.9 s of CPU in 8.9 s, Node 2.8 s in 9.4 s) and `jitstress-O2 jit pre-hot node` |
| `make test-web` | 114 PASS, 0 FAIL (with `tests/snapshot.mjs`'s new inflater and prefetch checks) |
| `node tests/input.mjs` | all passed |
| `make snapshot`, then `make test-snapshot-web` | pre-warm of Files, Calculator, Settings (each drawn); snapshot at 4% busy, image top 0x8155a000, 141.8 MiB plus 64.5 MiB of prefetch; restore, first frame, Settings drawn (64 colours), no ANR, fatal exception or fatal signal, no page errors; fallback for another build and for no snapshot: all PASS |

Not done in this round: the AC measurement of the final snapshot, and demand paging of the snapshot (item 2 of the
task). The soak (`tools/latency.mjs --runs 1 --idle 960 --calc`: 16 minutes untouched after the restore, then the
drawer, Settings and Calculator; battery, Low Power Mode on) did not finish: after the idle window the page did not
answer a click on Home within 30 s (`page.click: Timeout`), and the run's log was not kept. Whether the page or the
engine hung, and whether cached apps survive 15 guest minutes, is open. The runs that did finish lasted about 6
minutes, and no demo app was killed in them.

## Open issues, and the next-biggest cost

In order of what a visitor would notice:

1. **The first start of an activity: the next-biggest cost.** 10 to 16 s for Settings on AC (6 s for a Calculator
   process), against 1.8 s for a second start. In the browser the engine runs 50 to 70 M guest instructions per busy
   second during it (the native engine runs about 110 M) and the start is about 0.5 G instructions; with AOT apps the
   rate fell from 67 to 52 M per busy second. Things to measure next: the before tree at the new snapshot point (the
   control, about 30 min); in the engine's load report, the wasm JIT's translation work (blocks and modules compiled,
   and the host time it took, per interval) and the chunk reads (the AOT code's first run reads about 150 chunks of
   `Settings.odex`, AOT section), which tell whether the AOT code's first run is the cost; and the garbage collectors, which ran 30 to 50% of the instructions of a
   Settings start in the first runs (`HeapTaskDaemon` of Settings, system_server and the permission controller: the
   low-RAM heap settings, `dalvik.vm.heapminfree=512k` and `heaptargetutilization=0.75`, make many small collections).
   A pre-warmed snapshot (above) would turn the first start into a second one.
2. **Cached apps are killed.** The freezer's cause is fixed in P1: the engine's binder answers the freezer's ioctls
   with EINVAL and ActivityManager runs without the freezer (`Freezer disabled`). Not verified past about 6 minutes:
   the 16-minute soak ended with the page not answering a click (P1 verification).
3. **The media provider's restart loop**: fixed in B19 part 2 (docs/boot.md): the engine implements seccomp, the
   product runs the software codec2 store (`media.swcodec`) and no longer declares the hardware one. Left: the
   provider waits 5 s for `media.player` (no mediaserver) at its start, and its own roots (Images, Videos, Audio in the
   drawer) need the emulated external volume, which needs FUSE (`/dev/fuse`) in the engine. The Images, Audio, Videos
   row at the top of Files is DocumentsUI's search chips and does not depend on the provider.
4. **RemoteProvisioner** stays (the keymint HAL requires it, and the override did not remove it): its BOOT_COMPLETED
   receiver asks for a network and waits; 240 s later ActivityManager reports an ANR and kills it. With the snapshot at
   HOME + 240 s that ANR, with its stack dumps, came 44 s after every restore, during the first Settings start; the
   snapshot at HOME + 480 s is taken after it. Fix: drop the requirement (a keymint package of our own) or disable the
   receiver, so the boot does not need the wait.
5. **Not measured in this round:** `ro.config.low_ram` false against true, and the animation scales at 0.5. The page's
   `?prop=K=V` makes the first a snapshot parameter (`XN_SNAP_QUERY='&prop=ro.config.low_ram=false' make snapshot`);
   the second needs a boot-time `settings put` (the engine's init has no property triggers) or a SettingsProvider
   overlay for two of the three scales.
6. **The idle engine spins.** Fixed in P1 (`Atomics.wait`): 9 to 12% of a core for the idle phone, from 100%.
7. **Idle polling.** The idle phone still runs 4.8 M instructions per second: system_server's main thread, its
   `AdbDebuggingManager` thread (adb is on in a userdebug build, and there is no adbd to connect to), `SensorService`,
   servicemanager and a HAL. Turning adb off (`persist.sys.usb.config`, `ro.adb.secure`) is the first thing to try.
8. **The AOT comparison** (above): the "verify" variant of the demo product crash-loops SystemUI; the race
   ("No service published for: auth" when SystemUI's keyguard starts) is latent in the product too. With it measured,
   AOT could be kept for system_server and SystemUI and dropped for the apps a visitor opens, if issue 1 says so.
9. Headless Chromium uses SwiftShader for WebGL2; a run on a real GPU (a headed browser) was not made.
10. The pre-warmed snapshot: restores since P1 (it is the shipped snapshot).

## Verification

| Command | Result |
|---|---|
| `make && make wasm-node && make wasm` | built without warnings; `clang` and `emcc -Wall -Wextra -fsyntax-only` on the changed engine files: no new warning (`snap.c`'s `out_fd` warning under emcc is HEAD's) |
| `XIAOJI_JOBS=4 tests/run.sh` | **306 passed, 0 failed, 0 xfail** (with `snapwait-O0/O2 load` and `snapshot quiet`, engine and Node) |
| `make test-web` | all passed |
| `node tests/input.mjs` | all passed |
| `make snapshot`, then `make test-snapshot-web` | snapshot at HOME + 480 s, quiet at 4% busy, 115.8 MiB; restore, first frame, drawer, Settings, no ANR, fatal exception or fatal signal, fallback for another build and for no snapshot: all PASS |
| `XIAOJI_BOOT=1 XIAOJI_ROOT=build/rootfs-xiaoji tools/remote-test.sh run` (the build machine) | the product boot check passes (`sys.boot_completed` at 257 s, the launcher HOME at 270 s, 60 s alive); 265 passed, 4 failed: `simd-O0/O2` engine and Node differ from the recorded oracle there only (they pass on the Mac). `remote-test.sh all` stops at its gcc gate on two warnings in `mem.c` and `bpf.c` (not changed here), so `run` was used |
| `tools/remote-test.sh` | the open-file limit is raised to the hard limit for the run: with the build machine's 1,024 the product boot's PackageManager failed to open APKs and system_server died |
| `tests/input/settings.script` | the tap moved to Settings' new place (340, 201); not run natively in this round (no native snapshot of the new tree) |


### P1 on AC power (final engine and snapshot)

AC power, Low Power Mode off, 4.6–5.8 GB swap in use; headless Chromium (SwiftShader), localhost. After (P1) runs alternated
with before (L1 snapshot and engine, `prehot=0&inflaters=0&prefetch=0`); `build/p1/ac-final.sh`, log `build/p1/ac-final.log`.

| | before (2 runs) | after (3 runs) |
|---|---|---|
| press to first frame | 4.6–5.9 s | 2.8–4.2 s |
| drawer settled | 1.4–2.1 s | 0.8–1.2 s |
| first Settings start settled | 9.9–13.4 s | **1.7–2.0 s** |
| home settled | 2.2–3.8 s | 2.3–2.4 s |
| first Calculator start settled | 7.1–7.8 s | 1.7–2.6 s |
| idle engine busy | 9–19 % | 13–17 % |

The media provider still logs an ANR in two runs (`android.process.media`, a background process: no dialog is shown).

### B19 part 2: the media provider and the software codecs (docs/boot.md)

The engine enforces seccomp, the product runs `media.swcodec` and no longer declares the hardware codec2 store, and the
media provider starts once. The snapshot (`make snapshot` of these sources, the pre-warm as P1) was made with the
provider started at HOME + 57 s and never killed before the snapshot (the earlier make of the same engine on the old
tree killed it five times: "timeout publishing content providers"). The only ANR before the snapshot is
RemoteProvisioner's (open issue 4).

AC power, Low Power Mode off (`pmset -g batt`: AC Power, 100%, charged; `pmset -g | grep lowpowermode`: 0), swap
5.1 to 6.5 GB in use; headless Chromium, localhost; `tools/latency.mjs --runs 1 --warm --calc --files`, 3 runs, a new
browser each (`build/b19/ac-*.json`, logs `build/web-shots/l1-b19-*.log`). Against P1's AC numbers above (after, 3
runs; ranges):

| | P1 (AC) | **B19 (AC), median (range)** |
|---|---|---|
| press to first frame | 2.8 to 4.2 s | 3.1 s (2.7 to 3.4), 146.9 MiB |
| drawer settled | 0.8 to 1.2 s | 1.1 s (1.0 to 1.2) |
| first Settings start settled | 1.7 to 2.0 s | 2.0 s (1.8 to 2.4) |
| second Settings start settled | - | 1.8 s (1.6 to 1.9) |
| home settled | 2.3 to 2.4 s | 2.3 s (2.0 to 2.7) |
| first Calculator start settled | 1.7 to 2.6 s | 2.1 s (2.0 to 2.1) |
| **first Files start: response / settled** | 6.7 / 17.1 s (2.5 to 31.7; battery) | **3.1 / 3.7 s (3.7 to 4.1)** |
| idle engine busy | 13 to 17 % | 9 % (9 to 11) |
| idle browser host CPU | - | 10 % (9 to 12) |

In every run the provider process of the snapshot (pid 1604) answered DocumentsUI: no `Start proc` of the provider or
of `android.process.media`, no "timeout publishing content providers", no ANR, no fatal signal; Files' `Displayed` came
2.3 s after its START. `make test-snapshot-web`: all PASS (restore, first frame, Settings drawn with 64 colours, no ANR,
fatal exception or fatal signal; the fallbacks).

What Files shows is unchanged: the Downloads screen with the Images, Audio, Videos chips (DocumentsUI's search chips)
and "Can't load content at the moment" for the list, because the external volume is not mounted (no FUSE in the
engine; docs/boot.md, B19 part 2, open 2). The provider's own roots need that volume.

