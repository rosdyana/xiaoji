---
version: 1
slug: "web-index-html"
primary_target: "web/index.html"
related_targets: ["web/main.js"]
---

# Surface: the xiaoji page (web/index.html)

Mode: experience (the working phone leads), with a short persuade layer: visitors understand first, then press Start.
Audience: curious technical visitors from link aggregators. Job: see real Android in a tab, open apps, grasp the mechanism.
Must stay: XiaoJi boot screen and boot animation inside the phone (web/bootscreen.js, theme*.json); input on #android-screen; restore/boot logic in web/main.js and web/worker.js; dev cases behind ?cases=1, debug options.
Avoid: dev-tool clutter in the visitor's face (log, tables of debug state hidden by default), overhyped copy; only measured numbers with their test conditions.
Memorable moment: the phone as a live Figure 1 whose "This session" column fills in with the visitor's own timings.
Unresolved: mobile layout (desktop first); deploy headers.

## Direction contract

THESIS: XiaoJi is published like a semiconductor part's datasheet: a quiet, exact document whose Figure 1 is a working Android phone. It refuses the dark split-hero landing with a glowing device and a stat row.

OWN-WORLD: datasheet paper (#f4f3ee) and black ink, hairline rules, a 3px part-number strip, numbered section headings, tables with test conditions, pin callouts with leader lines, one XiaoJi green as the only accent; the phone stays dark. Workhorse sans for text, monospace only for measured values and pin labels.

STORY: the visitor reads "Real Android 13, running in this tab", sees the device, presses Start Android, watches their own timings land in the This session column, plays with apps, and can read the block diagram and limits.

FIRST VIEWPORT: part-number strip top (XJ-13, title, links, revision). Three columns: description + features left; the phone at exact scale as Figure 1 centre with Back/Home/keyboard callouts and Start Android plus download size beneath; Characteristics table (Measured vs This session ghost cells) and four-layer block diagram right. Footer rule with licences and known limits.

FORM: Component Datasheet, candidate 3 of 7 on my ordered list; seed:1cd715f7229ed603. Raises: ghost cells for unmeasured values (seven-segment); integer/exact screen scale (arcade); session events land once with real times and stay (passport); one vertical axis for the block diagram (deep dive); every restore/boot stage leaves a still record (raku).

FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, DESIGN.md, and every shipping raster carrying its provenance
