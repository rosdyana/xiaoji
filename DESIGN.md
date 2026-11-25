---
name: XiaoJi
description: Android 13 in a browser tab, published as a component datasheet whose Figure 1 is a working phone.
colors:
  paper: "#f4f3ee"
  paper-2: "#fbfaf6"
  text: "#141614"
  text-2: "#4f544e"
  rule: "#141614"
  hair: "#b9bcb3"
  seg-off: "#d5d8ce"
  green: "#17a35a"
  green-ink: "#0f7a42"
  green-wash: "#e3f1e8"
  sel: "#cbe8d6"
  led: "#3ddc84"
  device: "#0d1116"
  fault: "#8f2414"
typography:
  display:
    fontFamily: "Archivo, Helvetica Neue, Arial, sans-serif"
    fontSize: "32px"
    fontWeight: 760
    lineHeight: 0.9
    letterSpacing: "-0.01em"
    fontVariation: "'wdth' 88"
  headline:
    fontFamily: "Archivo, Helvetica Neue, Arial, sans-serif"
    fontSize: "clamp(28px, 2.3vw, 40px)"
    fontWeight: 720
    lineHeight: 1.08
    letterSpacing: "-0.02em"
    fontVariation: "'wdth' 86"
  title:
    fontFamily: "Archivo, Helvetica Neue, Arial, sans-serif"
    fontSize: "17px"
    fontWeight: 600
    lineHeight: 1.3
  subtitle:
    fontFamily: "Archivo, Helvetica Neue, Arial, sans-serif"
    fontSize: "15px"
    fontWeight: 650
    lineHeight: 1.5
  section:
    fontFamily: "Archivo, Helvetica Neue, Arial, sans-serif"
    fontSize: "13px"
    fontWeight: 700
    letterSpacing: "0.06em"
  body:
    fontFamily: "Archivo, Helvetica Neue, Arial, sans-serif"
    fontSize: "15px"
    fontWeight: 400
    lineHeight: 1.5
  table:
    fontFamily: "Archivo, Helvetica Neue, Arial, sans-serif"
    fontSize: "13px"
    fontWeight: 400
    fontFeature: "tnum"
  note:
    fontFamily: "Archivo, Helvetica Neue, Arial, sans-serif"
    fontSize: "12.5px"
    fontWeight: 400
    lineHeight: 1.5
  value:
    fontFamily: "Fragment Mono, ui-monospace, Menlo, monospace"
    fontSize: "12.5px"
    fontWeight: 400
  pin:
    fontFamily: "Fragment Mono, ui-monospace, Menlo, monospace"
    fontSize: "11.5px"
    fontWeight: 400
    lineHeight: 1.3
    letterSpacing: "0.02em"
rounded:
  none: "0px"
  key: "2px"
  button: "3px"
  screen: "18px"
  device: "30px"
spacing:
  pad-wide: "56px"
  pad: "36px"
  pad-phone: "16px"
  gap-wide: "40px"
  gap: "32px"
  gap-narrow: "28px"
  device-bezel: "12px"
components:
  button-primary:
    backgroundColor: "{colors.text}"
    textColor: "{colors.paper}"
    typography: "{typography.subtitle}"
    rounded: "{rounded.button}"
    padding: "11px 22px"
  button-quiet:
    backgroundColor: "transparent"
    textColor: "{colors.text}"
    rounded: "{rounded.button}"
    padding: "11px 22px"
  button-quiet-hover:
    backgroundColor: "{colors.paper-2}"
  pin-key:
    backgroundColor: "{colors.paper-2}"
    textColor: "{colors.text}"
    typography: "{typography.pin}"
    rounded: "{rounded.key}"
    padding: "5px 8px 5px 7px"
  pin-key-hover:
    backgroundColor: "{colors.text}"
    textColor: "{colors.paper}"
  diagram-layer:
    backgroundColor: "{colors.paper-2}"
    textColor: "{colors.text}"
    typography: "{typography.table}"
    rounded: "{rounded.none}"
    padding: "7px 10px"
  diagram-layer-accent:
    backgroundColor: "{colors.green-wash}"
    textColor: "{colors.text}"
    rounded: "{rounded.none}"
    padding: "6px 9px"
  device:
    backgroundColor: "{colors.device}"
    rounded: "{rounded.device}"
    padding: "{spacing.device-bezel}"
---

# Design System: XiaoJi

## Overview

**Creative North Star: "The Component Datasheet"**

The page is published the way a semiconductor part is: a quiet, exact document on datasheet paper, in black ink, with a part number at the top, numbered sections, a characteristics table with its test conditions, and a block diagram. Its Figure 1 is not a picture of a phone but the phone itself, running, drawn at an exact scale with pin callouts to its controls. Everything on the paper is there to describe that one part honestly.

Density is that of a real datasheet: small, tabular, hairline-ruled, with the eye led by rules and numbers rather than by size or colour. One green is the only accent, and it marks either the brand (the "-13" of the part number, the headline's second clause, the engine layer) or an interaction state. The device is the one dark object on the sheet, and inside it the XiaoJi boot screen keeps its own dark theme and animation.

The world rejects the dark split-hero landing with a glowing device and a stat row. Numbers appear only with their conditions; a value not yet measured is shown as an unlit display, never as a placeholder number.

**Key Characteristics:**
- Warm off-white paper, near-black ink, one green; the device is the only dark mass.
- A 3px part-number strip, then 1px black rules for structure and grey hairlines for rows.
- Numbered, uppercase, tracked section headings sitting on a black rule.
- Archivo (variable width, narrowed for display) for all text; Fragment Mono only for measured values, interfaces and pin labels.
- Square leader-line terminals (5-6px squares) on pins, list bullets, and the diagram's vertical axis.
- Seven-segment ghost readouts for values this session has not measured yet.

## Colors

Paper and ink with a single green, used like a spot colour on a printed sheet.

### Primary
- **XiaoJi Green** (green): structural accent lines and fills, the accent layer's 2px border in the block diagram, its axis tick, the accent step in How it works, and the outer ring of the primary button's hover halo.
- **Green Ink** (green-ink): the green as text and outlines on paper, where it must hold contrast: the "-13" in the part number, the headline's emphasised clause, link hover, focus outlines, caret.
- **Green Wash** (green-wash): the faintest green, as a fill: the accent diagram layer, the inner ring of the button hover halo, and the flash under a seven-segment cell the moment it lights.
- **Selection Green** (sel): text selection on paper.
- **LED Green** (led): the brand's Android green, used only on dark: "Android" in the Start button, and as the boot screen's accent inside the phone (its theme's `accent`).

### Neutral
- **Datasheet Paper** (paper): the page, the html background, and the browser theme colour.
- **Bright Paper** (paper-2): raised paper surfaces drawn with a full black border: diagram layers, pin keys, the developer panels.
- **Ink** (text): body text, headings, list bullets, leader lines, the diagram axis, the primary button's fill.
- **Second Ink** (text-2): secondary prose, captions, notes, section numbers, the second strip, footer, disabled keys.
- **Rule** (rule): structural 1px rules (section headings, table heads, strips, diagram boxes) and the 3px part-number strip. Same value as Ink; it is a separate token so rules can be tuned apart from text.
- **Hairline** (hair): row separators in tables and the session record, the disabled pin-key border.
- **Unlit Segment** (seg-off): the segments of a seven-segment readout that are not lit.
- **Device Black** (device): the phone's bezel, the only dark object on the paper.

### State
- **Fault Red** (fault): a failed stage or event in the session record (row text and its counter). Written as a literal in the build, not yet a custom property.

### Named Rules
**The Spot Colour Rule.** Green is the only accent on the paper; Fault Red appears only on a failed record row. Use Green Ink for green text and outlines on paper, XiaoJi Green for lines and borders, Green Wash for fills, LED Green only on the dark device or the black button.

**The One Dark Object Rule.** The device is the only dark surface on the page. Panels, tables and diagrams are paper with black rules, never dark cards.

## Typography

**Display Font:** Archivo (variable, width 62-125%, with Helvetica Neue, Arial)
**Body Font:** Archivo
**Label/Mono Font:** Fragment Mono (with ui-monospace, Menlo)

**Character:** A plain engineering grotesque, narrowed to 86-88% width at display sizes so the part number and headline read like a stamped title block; a quiet mono that appears only where a datasheet would print a measured value or a pin name.

### Hierarchy
- **Display** (760, 32px, 0.9, width 88%): the part number XJ-13 in the strip. One per page.
- **Headline** (720, clamp 28-40px, 1.08, width 86%, balanced): the single title line under the strips; 32px in one column.
- **Title** (600, 17px, 1.3): the part's long name in the strip.
- **Subtitle** (650, 15px): h3 entries in How it works, definition terms in Known limits, the primary button label (640).
- **Section** (700, 13px, 0.06em, uppercase): numbered section headings, the number in Second Ink at 500 before the label; 12px for the session record head.
- **Body** (400, 15px, 1.5): description, features (max 56-60ch), how-it-works steps (max 64ch).
- **Table** (13px, tabular figures): characteristics; table heads 12px/700.
- **Note** (12.5px, Second Ink): captions, test conditions (12px), notes, the second strip, footer, the session record rows.
- **Value** (Fragment Mono 12.5px): measured numbers in table cells and record times; 11px for interface labels in the block diagram.
- **Pin** (Fragment Mono 11.5px, 0.02em; keys 0.04em uppercase): pin callout labels and the BACK and HOME keys.

### Named Rules
**The Measured Mono Rule.** Monospace is for measured values, interface names and pin labels only. Prose, headings and buttons are Archivo.

**The Tabular Figures Rule.** Every place numbers line up (tables, notes, the strip's revision, the phone's stage count) uses tabular figures.

## Layout

The page is a printed sheet in bands: the part-number strip (3px rule below), a second strip of short facts (1px rule), a title block holding the one headline, then the first sheet in three columns: description and features left (1fr), Figure 1 centred at its natural width, characteristics and the block diagram right (1.12fr). Below the fold, a two-column band (1.25fr / 1fr, max 1440px) holds How it works beside Known limits and Ordering information, then a footer rule.

Side padding is 56px (36px under 1380px, 16px under 960px); column gap 40px (32px, then 28px). Section heads sit 12px above their content; below-the-fold sections are 40px apart.

The phone's screen is exactly 540 x 960 times a clean scale: 0.6 by default, 0.5 under 880px of height or 960px of width, 0.75 at 1500 x 1060 and up. The caption states the scale.

Responsive steps: under 1280px the sheet becomes two columns (text and characteristics left, figure right); under 960px one column (headline, figure, features, characteristics) and the strip wraps; under 600px the pin callouts drop their leader lines and sit as a row under the device.

**The Exact Scale Rule.** The device is never sized fluidly. It is 540 x 960 multiplied by 0.5, 0.6 or 0.75, and the figure caption names the factor.

## Elevation & Depth

The paper is flat. Depth is carried by rules: a 3px rule for the part-number strip, 1px black rules for structure, grey hairlines for rows, and a 1px black border around anything raised onto Bright Paper. The one exception is the device, which sits on the paper with a soft, low shadow, as a physical object photographed on the sheet.

### Shadow Vocabulary
- **Device on paper** (`box-shadow: 0 1px 0 rgba(255,255,255,0.06) inset, 0 22px 40px -22px rgba(20,22,20,0.55), 0 2px 6px rgba(20,22,20,0.12)`): the device bezel only.
- **Hover halo** (`box-shadow: 0 0 0 3px green-wash, 0 0 0 4px green`): the primary button on hover; a ring, not a lift.

### Named Rules
**The Rules Not Shadows Rule.** Nothing on the paper casts a shadow except the device. Separate and raise with rules and borders.

## Shapes

Square by default: tables, diagram layers, rules and panels have no radius. Interactive controls take the smallest softening (2px keys, 3px buttons). The device is the only rounded object (30px bezel, 18px screen), because it is a phone. Terminals are small filled squares, never dots: 5px pads where pin leaders meet the device, 6px squares for list bullets and on the diagram and how-it-works axes.

## Components

### Buttons
Black slab of ink, plain and certain.
- **Shape:** gently squared (3px).
- **Primary:** Ink fill, Paper text, 640 weight 15px, 11px 22px; the word "Android" lit in LED Green.
- **Hover / Focus:** a two-ring halo (Green Wash inside, XiaoJi Green outside), 120ms; focus is the global 2px Green Ink outline at 3px offset.
- **Quiet:** transparent with a black border (Stop); Bright Paper on hover.
- **Disabled:** 45% opacity.
- **Link button:** underlined 12px Second Ink text, Green Ink on hover (Show log).

### Pin keys
Device controls drawn as labelled pins of the part.
- **Style:** Bright Paper, 1px Ink border, 2px radius, Fragment Mono 11.5px uppercase, a 12px line icon and the pin number in Second Ink.
- **Hover:** inverts to Ink fill, Paper text; pressed moves down 1px.
- **Disabled:** Second Ink text, Hairline border, no fill.

### Pin callouts
A label, a 1px Ink leader line, and a 5px square pad touching the device's edge; left pins read left to right, right pins mirror. Secondary text under the label is 11px Archivo in Second Ink.

### Characteristics table
- **Style:** full width, 13px, tabular figures, 1px Rule under the head, Hairline between rows, numbers right-aligned in Fragment Mono.
- **This session column:** a seven-segment readout (SVG, 14px tall, skewed 7 degrees). Unmeasured, every segment is drawn in Unlit Segment; a measurement lights its segments in Ink once, with a 1100ms Green Wash flash that fades (no flash under reduced motion). The value never changes after it lands.
- **Notes:** test conditions and footnotes in 12px Second Ink directly beneath.

### Block diagram
One vertical 2px Ink axis with layers hanging off it on 20px ticks ending in 6px squares. Each layer is a Bright Paper box with a 1px Rule border, name in 600 left and implementation in 12px Second Ink right; interfaces between layers are 11px mono in Second Ink. The engine layer is the accent: 2px XiaoJi Green border, Green Wash fill, green tick.

### Session record
An ordered list under the figure: a 12px uppercase head on a 1px rule, rows on Hairlines with a counter, the event, and its time from the press in mono. Rows land once and stay. A failed row is Fault Red.

### Device (signature)
Device Black bezel (12px, 30px radius) holding the 18px-radius screen. Inside is the XiaoJi boot screen, which keeps its own theme from web/theme.json (dark gradient, LED Green accent, system UI font, the 小机 logo ring) and its boot animation (intro, spin and breathe loop, calm, outro over 600ms), with reduced and no-motion variants. Its styling is governed by the theme files, not by this paper system.

### Navigation
The part-number strip holds three underlined text links (600). Links on paper are Ink with a 1px underline at 3px offset; hover thickens to 2px and turns Green Ink.

## Do's and Don'ts

### Do:
- **Do** number every section heading and set it uppercase 13px/700 at 0.06em on a 1px black rule.
- **Do** print every performance number with its test conditions, and show unmeasured values as unlit seven-segment cells.
- **Do** keep the device at 540 x 960 times 0.5, 0.6 or 0.75 and say the scale in the caption.
- **Do** use Fragment Mono for measured values, interface names and pin labels, and Archivo for everything else.
- **Do** use small filled squares as terminals and bullets.

### Don't:
- **Don't** place a dark hero, a glowing device, or a row of big stat numbers on the page.
- **Don't** add a second accent hue; green is the only accent, and Fault Red marks failures only.
- **Don't** give paper surfaces shadows or rounded corners; only the device is rounded and shadowed.
- **Don't** restyle the boot screen inside the phone with paper tokens; it follows its theme files.
