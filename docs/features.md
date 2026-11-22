# Android features: base, add-on packs, engine-backed, or never

This is a draft for choosing what xiaoji ships in its base system and what it offers as optional packs, including "extended support" tiers.

## How a feature costs us

Delivery is lazy: users download only the chunks they touch. So a feature's files cost nothing until something opens them. What does cost:

1. **Boot time and snapshot size.** Processes and services that start at boot are part of the RAM snapshot every visitor downloads first. This is what the base tier must keep small.
2. **Host integration.** Camera, microphone, location, sensors, audio, GPU and network need bridges from Android's HALs to browser APIs. That's engine work, and it's the natural extended offering.
3. **Licensing.** Google apps and Play Services can't be redistributed.

## Model

- **One image contains everything.** A pack is not a different build. Turning a pack on enables its packages and services (`pm enable`, init service start), sets its `hasSystemFeature` flag (`/vendor/etc/permissions`), and prefetches its chunks. One build serves every combination, and toggles work at runtime.
- **Base:** the minimum set of running services, giving the smallest snapshot and the fastest start.
- **Add-on:** files in the image, services off by default, one switch to enable.
- **Engine-backed:** needs an engine bridge to a browser API. Opt-in, with a browser permission prompt where relevant. This is the extended-support candidate.
- **Never:** no sensible meaning in a tab, or not licensable.

## The list

Sizes are installed sizes in MiB from our `xiaoji_arm64` build's `installed-files*.json` (2026-11-22). Rows marked † were removed from that build; their size comes from the stock `sdk_phone64_arm64` build. **Boot cost** (processes and RAM at boot) gets filled in once Android boots under the engine. The "Starts at boot" column is what's known from Android itself.

### Core system (always base)

| Feature | Size | Starts at boot | Browser backing | Proposed |
|---|---|---|---|---|
| Framework, ART, bionic, boot image (`/system/framework`, libs, ART and runtime APEXes) | ~430 | zygote, system_server | — | base |
| ICU and time zones (i18n, tzdata APEXes) | 33 | (in every process) | — | base |
| SystemUI (status bar, notifications, quick settings) | 33 | yes (persistent) | — | base |
| Launcher3 | 20 | yes (home) | — | base |
| Settings | 67 | no | — | base |
| PackageInstaller (APK upload and install) | 3 | no | file upload | base |
| DocumentsUI and file providers (files in and out of the tab) | 6 | no | File System Access / upload and download | base |
| Permission controller (permission APEX) | 14 | on demand | — | base |
| Media provider (photos and files index) | 10 | on demand | — | base |
| Core providers (settings, contacts, downloads, calendar storage) | ~4 | on demand | — | base |
| **Keyboard (LatinIME)** † | 18 | yes (IME) | browser keyboard events | **base: it was dropped with the product apps by mistake; restore it, or bridge the browser's keyboard as an IME** |
| framework-res (every locale and density) | 57 | (loaded by every app) | — | base (lazy) |

### Text and fonts

| Feature | Size | Starts at boot | Browser backing | Proposed |
|---|---|---|---|---|
| Latin and other Noto fonts | 16 | mapped by zygote | — | base (lazy pages) |
| CJK fonts | 44 | mapped by zygote | — | base (lazy pages; only touched glyph pages download) |
| Emoji | 12 | mapped by zygote | — | base (lazy) |
| Hyphenation data | 2 | no | — | base (lazy) |

### Networking

| Feature | Size | Starts at boot | Browser backing | Proposed |
|---|---|---|---|---|
| Network stack, connectivity (tethering APEX), DNS (resolv) | ~12 | yes | engine network: xiaonux-style relay or helper | engine-backed (off = offline device) |
| Wi-Fi stack (wifi APEX) | 8 | yes | none; present the relay as Ethernet | never (replace with an Ethernet-style link) |
| VPN dialogs | <1 | no | needs network | add-on, with network |
| Captive portal login | <1 | no | — | never |

### Phone, radios and hardware

| Feature | Size | Starts at boot | Browser backing | Proposed |
|---|---|---|---|---|
| Telephony (TeleService, Telecom, TelephonyProvider, radio HALs) | ~14 | yes (`com.android.phone` is persistent) | — | never in base; Android still expects Telecom, so ship it disabled or "no telephony" (tablet mode) |
| Cell broadcast (APEX and legacy app) | 14 | yes | — | never |
| Dialer, Contacts, Messaging † | 35 | no | — | add-on ("AOSP apps" pack), without telephony |
| Bluetooth (btservices APEX) | 19 | yes | Web Bluetooth is too limited | never |
| NFC, secure element, UWB, Tag † | ~13 | yes (NFC and SE are persistent) | Web NFC is Android-only | never (SecureElement is still pulled in; remove it) |
| Camera (cameraserver, Camera2 app †) | ~10 | cameraserver at boot | `getUserMedia` video | engine-backed |
| Microphone and audio (audioserver, media APEXes, software codecs) | ~5 | yes | Web Audio, `getUserMedia` audio | engine-backed (audio out could become base later) |
| Location (FusedLocation, GNSS HAL) | <1 | yes | Geolocation API | engine-backed |
| Sensors (accelerometer, gyroscope) | — (vendor HAL) | yes | DeviceMotion / DeviceOrientation | engine-backed |
| Vibration | — | — | Vibration API | engine-backed (trivial) |
| GPU acceleration (GLES → WebGL2/WebGPU) | — | — | WebGL2 / WebGPU | engine-backed, but effectively required for a smooth UI; base once it exists |
| Neural networks (NNAPI APEX) | 4 | no | WebNN, later | never for now |

### Apps and UI extras

| Feature | Size | Starts at boot | Browser backing | Proposed |
|---|---|---|---|---|
| WebView (Chromium) | 170 | no (loaded by apps that use it) | — | add-on, or base-lazy: many apps need it; files only download when an app opens a WebView |
| AOSP apps: Calendar, Clock, Camera, Gallery, Music † | ~27 | no | — | add-on ("AOSP apps" pack) |
| Live wallpapers, wallpaper picker, dreams † | ~5 | no | — | add-on (personalisation pack) |
| Printing (PrintSpooler, built-in print service) † | ~2 | PrintSpooler at boot | `window.print()`, later | add-on |
| EasterEgg, QuickSearchBox, PhotoTable † | ~6 | no | — | never |
| Storage manager | 11 | no | — | never (the snapshot controls storage) |

### Platform services

| Feature | Size | Starts at boot | Browser backing | Proposed |
|---|---|---|---|---|
| ADB (adbd APEX) | 3 | adbd | engine pipe; DevTools-style console | add-on ("developer" pack: adb shell, logcat, file push) |
| Tracing (Traceur) † | 12 | no | — | add-on (developer pack) |
| Multi-user, work profile (ManagedProvisioning †) | 6 | no | — | never |
| Backup (backup transport) | <1 | no | — | never (the snapshot plus OPFS is our backup) |
| Ad services, on-device personalisation APEXes | ~3 | partly | — | never |
| DRM: Widevine (not in AOSP), ClearKey | — | mediadrm | EME can't be reached from the guest | never |
| Google Play Services, Play Store | — | — | — | never (licensing); a microG-style alternative could be an add-on after a licence review |
| Statsd, extservices, scheduling and similar mainline modules | ~10 | some | — | base unless measured as costly |

### Vendor side (goldfish emulator HALs today)

| Feature | Size | Starts at boot | Browser backing | Proposed |
|---|---|---|---|---|
| Goldfish vendor (radio, GNSS, sensors and camera emulation, QEMU pipes) | 85 | many HAL services | — | replace with xiaoji's own HALs, which bridge to the engine; drop what isn't bridged |
| VNDK APEX (vendor-facing copies of system libraries) | 27 | — | — | drop once our own HALs need only what's in it, or link them against system libs |

## What the build should change next

1. **Restore a keyboard.** LatinIME was removed together with the product apps. Either add LatinIME back to base, or write a small IME that receives the browser's keyboard input from the engine. The second option is better on desktop, because a physical keyboard has nothing to tap.
2. Remove SecureElement, cell broadcast, the Wi-Fi APEX, UWB, ad services and on-device personalisation from what starts at boot. Set `config_voice_capable=false` and similar so the phone process doesn't run.
3. Once Android boots under the engine, measure the boot cost per row (processes, RSS, snapshot pages) and revisit the "Proposed" column with numbers.

Open decisions are marked by the "Proposed" column; change any row you disagree with.
