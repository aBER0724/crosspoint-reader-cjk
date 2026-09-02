# Fork Feature Preservation Guide

**Languages:** [English](fork-features.md) · [简体中文](fork-features-ZH.md) · [日本語](fork-features-JA.md)

## Purpose

This guide records fork behavior and implementation constraints that must survive upstream merges.
Review behavior—not only APIs or build results—when resolving conflicts in the paths below.

## User-visible fork features

### 1. CJK interface and glyph coverage

- The shipping interface supports Simplified Chinese, Traditional Chinese, Japanese, and English.
- SC, TC, and JA translations cover complete device workflows, including font management, updates, errors, and recovery-facing labels.
- Built-in CJK UI glyphs are generated from the shipping translation corpus and required UI symbols rather than maintained as an ad hoc hand list.
- SC and TC release environments retain the intended default language and generated font coverage.
- Missing UI glyphs may fall back to a compatible SD font without changing Latin UI metrics unexpectedly.

Related history: `2d1148ff`, `619b6dcd`, `962c06ff`, `52514474`.

### 2. Independent reader and UI SD fonts

- Reader text and device UI have separate SD `.cpfont` family roles.
- Selecting, replacing, clearing, or failing to load one role must not silently overwrite the other.
- Built-in families remain valid fallbacks, but a built-in reader family is not automatically exposed as an SD-backed UI family.
- UI fallback registration requires usable CJK coverage and exact-size behavior suitable for interface metrics.
- Font selection survives catalog entry/exit, low-memory recovery, restart, and rediscovery of the SD registry.

Related history: `06c2686a`, `4c3c40ce`, `ab5cfb6d`, `dfc5fc54`.

### 3. On-device font catalog and safe installation

- The device supports multiple font repositories, including configured owner/repository specifications and HTTPS manifest URLs.
- Users can browse catalog metadata, see update timestamps, preview remote families, and install selected sizes or complete families.
- Reader-family and UI-family packages use staged, transactional replacement; interrupted or failed installs recover without destroying the last usable family.
- Browser uploads of font families follow the same transactional principle.
- Downloaded files are bounded, structurally validated as canonical CPFont v4 where required, and verified against manifest SHA-256 values.
- Large manifests are parsed as streams; manifest memory is released before large font downloads and rendering work.
- HTTPS redirect handling, TLS buffer pressure, and range downloads are constrained for ESP32-C3 heap limits.
- Cancellation remains available during network, verification, preview, and install work.
- Low-memory self-healing must preserve the configured UI family and must not strand temporary preview or replacement files.

Related history: `66dd6348`, `8ea12890`, `0eb39e3e`, `4c2016f2`, `dc9e2f6b`, `6654c172`, `ccedc6ef`.

### 4. CJK reader typography and responsiveness

- EPUB and TXT layout use CJK-aware line breaking and punctuation attachment rather than Latin whitespace assumptions.
- Justification treats CJK break opportunities as stretchable gaps without inserting visible Latin spaces.
- Optional first-line indentation is approximately two ideographs and participates in pagination/cache invalidation.
- Wider CJK point-size families are selectable by actual installed size, not by a fragile ordinal slot.
- SD glyph reads are batched or prewarmed to avoid a random SD seek for every character on a CJK page.
- Indexing status is displayed before blocking work begins.
- Section building, glyph collection, image decoding, grayscale rendering, indexing, and page rendering remain cancellable at safe boundaries.
- Input and activity transitions take priority over stale background work; a stale render must not later overwrite the new screen.

Related history: `4401cf40`, `15a7959a`, `a3e39099`, `d10e84cb`, `0db37e73`, `775ef4cc`.

### 5. Dark-mode display correctness

- Visible dark-mode updates use `DarkRedrive`, including ordinary page changes, recovery/fallback paths, modal screens, sleep transitions, and low-memory degradation.
- Dark mode must not fall through to `HALF_REFRESH` or ordinary `FAST_REFRESH` merely because an upstream path does so in light mode.
- Partial/window updates use the dark-redrive window path and transform logical windows into the correct physical panel coordinates.
- Reader images preserve intended polarity: the dark background may be inverted without accidentally double-inverting covers or page images.
- Cropped cover bounds are completely filled so stale border pixels do not remain visible.

Related history: `76c0a1bc`, `d720891b`, `d92649d5`, `489e028a`, `2b79de3d`.

### 6. UI portrait inversion and orientation-aware input

- Reader orientation and UI orientation are separate persisted settings.
- The reader supports its full orientation set; normal UI activities intentionally support Portrait and Portrait Inverted.
- UI inversion applies to the complete interface, not only the reader: Home, Settings, dialogs, keyboard, Wi-Fi, font catalog, and update screens must enter with the UI orientation.
- Input mapping follows the orientation actually rendered, not a stale reader setting.
- Button hints, safe areas, battery/status placement, selection updates, and touch/logical coordinates remain correct when inverted.
- Do not “simplify” the two settings into one enum or make Home inherit landscape reader orientation.

Related history: `106af11f`, `47b838ff`, `78154ba9`, `39889436`.

### 7. Deferred and cancellable Home covers

- Home becomes interactive before expensive missing-cover work is completed.
- Cover thumbnail generation is deferred, cancellable on input/activity changes, and reusable when a valid cached thumbnail already exists.
- Render generations and snapshots prevent stale cover work from painting after navigation or geometry changes.
- Partial selection updates start from a known panel baseline and update only safe regions.
- Dark-mode cover polarity and buffer restoration remain correct during partial updates.
- Cache misses or interrupted renders degrade to a complete, usable Home screen rather than a half-painted card.

Related history: `27ae3243`, `e75102a4`, `9b27173a`, `9beff5d0`, `5f9854d8`.

### 8. Responsive Web and upload paths

- Long HTTP/WebDAV/file-list operations feed or safely coordinate with the task watchdog and yield where appropriate.
- Web responses that can outlive a handler use durable state; do not capture short-lived buffers or objects in deferred callbacks.
- Settings-page data loading and response production remain nonblocking enough for the main loop and display/input work.
- Upload paths stream to storage with bounded buffers and retain a low-memory path; they must not require file-sized RAM.
- Partial, timed-out, oversized, or failed uploads do not replace a valid destination.
- SD access from Web, WebDAV, rendering, fonts, and persistence follows the repository's serialization/locking rules.

Related history: `a946c83a`, `27ec21db`, `128d5236`, `f6deae44`.

### 9. Wi-Fi scan recovery and CJK-region hotspot discovery

- A new asynchronous scan clears stale Arduino Wi-Fi scan state before starting.
- Failed connections, interrupted scans, and mobile-hotspot changes can recover without rebooting.
- Before association, scanning is seeded with country code `CN` so channels 12 and 13 remain discoverable.
- IEEE 802.11d remains enabled so the radio can adopt the access point's advertised regulatory domain.
- Do not replace this with a permanent regulatory lock or remove it as an apparently regional constant.

Related history: `accf1ee0`.

### 10. SD performance and serialization

- FileBrowser avoids repeated full-path/text work and unnecessary SD reads in hot list-rendering paths.
- SD-backed UI fonts and reader fonts retain cache/prewarm behavior that limits random access and heap churn.
- File close/open sequences that interact with shared SD state remain serialized; a harmless-looking early close can race another task.
- Long directory walks and file copies periodically yield/feed the watchdog.
- Error and cancellation paths release font/file resources without leaving the registry or UI font state inconsistent.

Related history: `16a59f4a`, `361aee32`, `c9aad6db`, `f6deae44`.

### 11. SD firmware, recovery, and safe upload

- Users can install firmware from SD through the normal settings flow.
- Holding the documented button combination at boot can enter the SD firmware picker when normal UI routing is unavailable.
- The standalone recovery environment and package flow remain buildable; see [SD Recovery Firmware](recovery-firmware.md).
- Firmware images are validated for image format, target chip/device, partition compatibility, and safe size before writing.
- Upload/install code preserves required bootloader, partition-table, factory, and dual-OTA layout assumptions.
- Successful installation boots the intended OTA image; failures leave a recoverable device and a useful on-screen error.
- Firmware transfer is streamed and watchdog-safe rather than buffered as one large allocation.

Related history: `5717374e`, `bf796a63`, `3e993191`, `e0ff0c9a`.

### 12. Persisted settings and Calibre cleanup

- Existing saved settings are a compatibility boundary: preserve JSON keys, enum numeric meanings, defaults, negative SD font IDs, and migration behavior.
- In particular, reader orientation and UI orientation indices, sleep-screen enum order, keyboard layout bits, and font-role selections must remain stable.
- New fields need backward-compatible defaults and registration in the settings schema/list.
- Reader progress and crash-recovery writes may be deferred for responsiveness, but are flushed before deep sleep or another persistence boundary.
- Calibre connection UI must not display a stale administrative token left by an older session or removed authentication flow.

Related history: `6e4d0e53`, `0299a89c`, `ee3d714d`, `b0049cba`.

## Merge invariants

The items above describe what users receive. The following are implementation-level invariants used to preserve it:

1. Generated files remain generated. Change translation YAML, glyph-generation inputs, or generator scripts, then regenerate.
2. Reader-font and UI-font IDs/state remain independent through settings, registry discovery, renderer setup, preview, install, and rollback.
3. A font package becomes active only after complete download, size/hash validation, CPFont validation, and atomic/staged replacement.
4. Manifest and font downloads have explicit byte limits, cancellation checks, watchdog servicing, and bounded-memory streaming.
5. Dark-mode visible refresh decisions explicitly route to buffer or window `DarkRedrive`; no implicit light-mode fallback is acceptable.
6. Logical-to-physical window transforms and image polarity are preserved across every supported orientation.
7. Long render/index/cover jobs carry a render generation or cancellation predicate and check it before committing the framebuffer.
8. Main-loop input polling is not moved behind blocking rendering, networking, display waits, or SD operations.
9. Shared SD operations use `HalStorage`/repository locking conventions; avoid raw concurrent SD handles across tasks.
10. Persisted enum values and JSON keys are data-format APIs, even when the C++ declaration looks private.
11. Firmware writes validate before erasing/writing and preserve the boot/partition contract for supported Xteink hardware.
12. Sensitive or session-derived Web/Calibre values are cleared when their owning session ends.

## High-risk source paths

| Area | Paths to review after upstream merges |
|---|---|
| CJK i18n/glyph generation | `lib/I18n/translations/{chinese_simplified,chinese_traditional,japanese}.yaml`, `scripts/gen_i18n.py`, `scripts/generate_cjk_ui_font.py`, `scripts/check_cjk_ui_font_charset.py`, `lib/GfxRenderer/cjk_ui_font_*.h` |
| Font roles and registry | `src/SdCardFontSystem.*`, `src/CrossPointSettings.*`, `src/SettingsList.h`, `lib/EpdFont/SdCardFont*`, `lib/GfxRenderer/FontCacheManager.*` |
| Catalog/install/download | `src/activities/settings/Font*`, `src/FontInstaller.*`, `src/FontRepositoryStore.*`, `src/util/FontManifest.*`, `src/util/FontRepositoryUtil.*`, `src/network/HttpDownloader.*`, `src/CpfontValidator.*` |
| Reader layout/responsiveness | `lib/Epub/Epub/ParsedText.*`, `lib/Epub/Epub/Section.*`, `lib/Epub/Epub/Page*`, `src/activities/reader/EpubReaderActivity.*`, `src/activities/reader/TxtReaderActivity.*`, `src/activities/ActivityManager.*` |
| Display/dark mode | `lib/ReaderRuntime/ReaderRuntimePolicy.*`, `lib/GfxRenderer/GfxRenderer.*`, `lib/hal/HalDisplay.*`, `src/activities/reader/ReaderUtils.h`, `src/activities/boot_sleep/SleepActivity.*` |
| Orientation/input | `src/OrientationHelper.h`, `src/MappedInputManager.*`, `src/components/themes/*`, all activity entry/render paths |
| Home covers | `src/activities/home/HomeActivity.*`, `src/components/themes/lyra/Lyra*`, `src/RecentBooksStore.*`, EPUB/TXT/XTC cover generation |
| Web/SD concurrency | `src/network/CrossPointWebServer.*`, `src/network/WebDAVHandler.*`, `lib/hal/HalStorage.*`, `src/activities/home/FileBrowserActivity.*` |
| Wi-Fi | `src/activities/network/WifiSelectionActivity.*`, Wi-Fi connection helpers and credential store |
| Firmware/recovery | `src/network/FirmwareInstaller.*`, `src/network/SdFirmwareUpdater.*`, `src/activities/settings/SdFirmwareUpdateActivity.*`, `src/recovery/*`, `src/main.cpp`, `platformio.ini`, partition files and upload scripts |
| Persistence/Calibre | `src/CrossPointSettings.*`, `src/SettingsList.h`, settings/state stores, `src/JsonSettingsIO.*`, `src/activities/network/CalibreConnectActivity.*` |

## Concise post-merge checklist

- [ ] Build both SC and TC release environments; regenerate i18n and CJK UI fonts from source inputs.
- [ ] Boot in EN, SC, TC, and JA; inspect Home, Settings, font catalog, Wi-Fi, update, errors, and dialogs for missing glyphs.
- [ ] Select different reader/UI SD families, reboot, enter/exit catalog, simulate a failed replacement, and confirm both roles survive.
- [ ] Preview and install from at least two repositories; test cancellation, bad SHA-256, invalid CPFont, interrupted install, and low heap.
- [ ] Open CJK-heavy EPUB/TXT content; test punctuation wrapping, justification, two-ideograph indent, indexing status, and cancellation during page work.
- [ ] Exercise light/dark page turns, images, covers, sleep, dialogs, low-memory fallback, and partial/window updates; look for whitening or reversed polarity.
- [ ] Test reader rotations separately from Portrait/Portrait Inverted UI; verify hints, buttons, touch/logical coordinates, and Home orientation.
- [ ] Navigate Home immediately while covers are missing; confirm input cancels stale work and no later cover overwrites the active screen.
- [ ] Stress Web file lists/settings/WebDAV/uploads while using the device; check watchdog, heap, partial-file cleanup, and SD serialization.
- [ ] Rescan after a failed hotspot connection and verify a channel 12/13 hotspot can be found without rebooting.
- [ ] Test SD firmware install, boot-button recovery entry, invalid/wrong-device images, and successful reboot into the new image.
- [ ] Load pre-merge settings/state, verify enum meanings and font IDs, enter deep sleep, and confirm progress/recovery state is committed.
- [ ] Enter and leave Calibre connection flows and confirm no stale administrative token is shown.
- [ ] Run format, static analysis, default build, and relevant host-side contract tests before accepting the merge.

## Maintaining this document

Update this guide when a fork feature is intentionally removed, replaced, or upstreamed with equivalent tested behavior.
Prefer adding one representative commit and the narrowest high-risk path over recording every fix in a long series.
Keep all three language versions semantically aligned, and verify the navigation links whenever a file is renamed.
