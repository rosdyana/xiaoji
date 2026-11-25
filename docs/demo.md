# The demo product and its yardstick (B19)

Goal: a visitor's first minute. Boot Android restores a booted snapshot; then the home screen, the app drawer, Settings
and the other apps should respond like a phone's. Cold boot time does not matter here. This note is the product
(`aosp/device/xiaoji`), what was removed and why, the AOT question, the snapshot point, and the measurement method with
the numbers before and after.

Status: **done, with open issues** (the end of this note). The product has Calculator and seven fewer apps that ran,
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

**Pre-warm** (`XN_SNAP_PREWARM=on`; off by default): before the snapshot, Files, Calculator and Settings are each
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
  `media.documents` roots, the process starts, waits for the codec2 HAL (`android.hardware.media.c2@1.1::IComponentStore/default`
  is declared in the goldfish vendor manifest and never started), is killed after 40 s ("timeout publishing content
  providers"), and starts again at the next request. Removing it is not possible without the APEX (its
  `framework-mediaprovider.jar` is on the boot class path), and Files, downloads and SystemUI's screenshots use it.
  Starting the two codec2 stores instead (`android.hardware.media.c2@1.0-service-goldfish` and the swcodec APEX's
  `mediaswcodec`, in a trial boot on the build machine) fails: both install a seccomp filter through minijail, the
  engine answers `prctl(PR_SET_SECCOMP)` with EINVAL, and they abort. Open issue below.
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
2. **Cached apps are killed.** The framework's cached-app freezer is on ("Freezer enabled": the engine's cgroup tree
   has `cgroup.freeze` and answers `BINDER_GET_FROZEN_INFO`), but the engine has no `BINDER_FREEZE`, so ActivityManager
   kills each cached process about 10 guest minutes after it became cached ("Unable to freeze binder interface"): in
   the after runs, Provision, external storage, KeyChain and `android.process.media` died about 5 minutes into the
   session, and a pre-warmed app would die the same way. Fix: the engine answers the freezer's binder check as
   unsupported (the framework then does not freeze), or implements freezing.
3. **The media provider's restart loop** (above): it waits for a codec2 HAL that is declared and not started, is
   killed after 40 s, and starts again when DocumentsUI asks for it. Fix: the engine accepts a seccomp filter
   (`prctl(PR_SET_SECCOMP)`, `seccomp(2)`) without enforcing it, and the boot config starts the two codec2 stores; or a
   device manifest without the `IComponentStore/default` entry, and `mediaswcodec` started.
4. **RemoteProvisioner** stays (the keymint HAL requires it, and the override did not remove it): its BOOT_COMPLETED
   receiver asks for a network and waits; 240 s later ActivityManager reports an ANR and kills it. With the snapshot at
   HOME + 240 s that ANR, with its stack dumps, came 44 s after every restore, during the first Settings start; the
   snapshot at HOME + 480 s is taken after it. Fix: drop the requirement (a keymint package of our own) or disable the
   receiver, so the boot does not need the wait.
5. **Not measured in this round:** `ro.config.low_ram` false against true, and the animation scales at 0.5. The page's
   `?prop=K=V` makes the first a snapshot parameter (`XN_SNAP_QUERY='&prop=ro.config.low_ram=false' make snapshot`);
   the second needs a boot-time `settings put` (the engine's init has no property triggers) or a SettingsProvider
   overlay for two of the three scales.
6. **The idle engine spins.** In the browser build the scheduler's idle sleep is Emscripten's `nanosleep`, a busy loop
   without pthreads: an idle phone keeps one host core at 100%. `Atomics.wait` on a shared buffer (which the page could
   also `notify` when input arrives) would make the idle worker cost nothing.
7. **Idle polling.** The idle phone still runs 4.8 M instructions per second: system_server's main thread, its
   `AdbDebuggingManager` thread (adb is on in a userdebug build, and there is no adbd to connect to), `SensorService`,
   servicemanager and a HAL. Turning adb off (`persist.sys.usb.config`, `ro.adb.secure`) is the first thing to try.
8. **The AOT comparison** (above): the "verify" variant of the demo product crash-loops SystemUI; the race
   ("No service published for: auth" when SystemUI's keyguard starts) is latent in the product too. With it measured,
   AOT could be kept for system_server and SystemUI and dropped for the apps a visitor opens, if issue 1 says so.
9. Headless Chromium uses SwiftShader for WebGL2; a run on a real GPU (a headed browser) was not made.
10. The pre-warmed snapshot (above) does not restore in headless Chromium yet.

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

