# Product

<!-- impeccable:product-schema 1 -->

## Platform

web

## Users

Curious technical visitors, mostly arriving from link aggregators and social posts (Hacker News, Reddit, X). They open android.xiaonux.dev to see whether real Android actually runs in a browser tab, play with it for a few minutes (home screen, app drawer, Settings, Calculator, Files), and want to understand how it works.

## Product Purpose

xiaoji runs Android 13 in a browser tab with nothing to install. Success is a visitor who sees the home screen within seconds, opens apps that respond like a phone, and leaves convinced it is real Android, with a clear idea of the mechanism behind it.

## Positioning

No virtual machine: a user-mode ARM64 engine written in C and compiled to WebAssembly runs Android's own user-space binaries and provides the Linux kernel interface itself (syscalls, binder, input devices, the emulator graphics pipe); OpenGL ES goes to WebGL2. A booted snapshot restores straight to the home screen, delivered as content-addressed chunks. Sibling of xiaonux (Debian in a browser tab, full RISC-V machine emulation).

## Operating Context

- Desktop browser first (Chromium-based; needs cross-origin isolation, SharedArrayBuffer, WebGL2). Mobile browsers are a later concern.
- First visit downloads the snapshot (~140 MB today, cached afterwards); restore to home in a few seconds on a fast connection; cold boot (minutes) is the fallback when no matching snapshot exists.
- Input: mouse/touch on the phone screen, physical keyboard (no on-screen IME yet); Back and Home exist as page buttons.
- Android 13 product: trimmed AOSP (launcher, SystemUI, Settings, Files, Calculator); network is off.

## Capabilities and Constraints

- Page surface (web/): the phone screen (canvas `#android-screen`), the XiaoJi boot screen and animation, device controls, a log, developer test cases behind `?cases=1`, debug options (`?debug=1`, `?snapshot=off`).
- Wanted on the page: short intro and how it works, device controls around the phone, a live stats panel (restore/boot timings, download progress, CPU/FPS, log drawer), links and credits (GitHub, xiaonux, licenses/NOTICE, known limits).
- Display is 540×960 at 240 dpi (engine/display.h); input maps through the letterboxed picture (web/input.js).
- Plain static HTML/CSS/JS, no framework or build step for the page; served with COOP/COEP headers.
- Undecided: APK install, mobile layout, accounts — none exist; do not imply them.

## Brand Commitments

- Name XiaoJi (小机, "little phone"); the 小机 glyph in the logo ring; green accent; the existing XiaoJi boot screen and its boot animation (web/bootscreen.js, theme*.json, intro/loop/outro) are kept. Themes stay customizable.

## Evidence on Hand

- Real measurements in docs/demo.md (AC power, headless Chromium): press to home 2.8–4.2 s, first Settings open 1.7–2.0 s, Calculator 1.7–2.6 s, drawer 0.8–1.2 s.
- Screenshots in build/web-shots/ (launcher, drawer, Settings, Calculator 1+2=3).
- Open-source repository github.com/rosdyana/xiaoji (Apache-2.0). No users, testimonials, press or benchmarks against other products exist; do not invent them. Twoyi Web is a reference competitor but no comparative numbers are published.

## Product Principles

1. The phone is the hero: get the visitor to a working Android screen as fast as possible; everything else is secondary.
2. Honest: show real progress and real numbers, never fake speed or claims.
3. Explain the mechanism briefly for the curious, without getting between the visitor and the phone.
4. Feels like a device: controls behave like a phone's, input is direct.

## Accessibility & Inclusion

Respect prefers-reduced-motion (boot animation already does); keyboard-operable page controls; text alternatives for the phone screen's state (loading, restoring, error).
