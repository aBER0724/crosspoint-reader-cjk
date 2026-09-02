# CLAUDE.md

Development instructions for Claude and other assistants in `crosspoint-reader-cjk`.
Read `AGENTS.md` first; it is the authoritative repository guide. This file highlights the rules most likely to be missed during automated work.

## Repository identity

This is the CJK-focused CrossPoint Reader fork, not a stock upstream checkout. It targets memory-constrained Xteink ESP32-C3 hardware and also keeps the ESP32-S3 `sticky` target buildable. Upstream code may be a useful baseline, but upstream behavior does not automatically supersede this fork's tested behavior.

Before work begins:

```bash
git branch --show-current
git remote -v
git status --short
git submodule status --recursive
```

Preserve unrelated changes. Do not assume branch or remote names. Never rewrite shared history, force-push, or perform destructive device flashing without explicit authorization.

## Submodules are build inputs

Initialize recursively:

```bash
git submodule update --init --recursive
```

- `freeink-sdk/` is mandatory. `platformio.ini` references its libraries with local `symlink://` dependencies; a non-recursive or missing checkout will not produce a valid build.
- `freeink-sdk/libs/assets/Icons/lucide` is a nested source asset submodule used when regenerating Lucide-derived icons. Ordinary compilation can use the existing generated icon headers, but recursive initialization keeps the SDK checkout complete and matches CI.
- The recorded SDK gitlink belongs to this fork. Do not silently advance it to another SDK branch or upstream HEAD.
- Verify uncertain display, storage, network, board, and input APIs against the checked-out SDK source before using them.
- SDK implementation changes and the main-repository gitlink update should be deliberate and separately reviewable.

## Build environments

Use the correct environment:

- `default`: normal ESP32-C3 development firmware; EN/SC/TC/JA.
- `gh_release` / `gh_release_rc`: Simplified Chinese release line; EN/SC/JA.
- `gh_release_tc` / `gh_release_rc_tc`: Traditional Chinese release line; EN/TC/JA.
- `slim`: C3 size-focused build without serial logging.
- `device_test`: device automation with serial input injection; test-only and never a release artifact.
- `recovery`: minimal English SD recovery firmware with a separate source filter.
- `sticky`: ESP32-S3 target used by CI; it is not an X4-compatible binary.

Useful commands:

```bash
./bin/clang-format-fix -g
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
pio run -e default -e sticky
pio run -e gh_release
pio run -e gh_release_tc
pio run -e recovery
pio run -e default --target upload
pio device monitor
```

CI intentionally builds `default` and `sticky` in one `pio` invocation. Reproduce that shape when checking CI failures because separate invocations can invalidate artifacts as toolchains change.

For Xteink C3 uploads, retain the registered safe upload path. It writes application partitions and updates OTA selection while preserving the bootloader, partition table, and data partitions such as NVS, SPIFFS, and coredump; it does not preserve old factory/OTA application images. After testing `device_test` or temporary screenshot configurations, remove temporary overrides and restore `default` firmware.

`platformio.local.ini` is machine-local and gitignored. Never commit it. Do not use `git clean -fdX`; it can remove this file and other intentionally ignored assets.

## Generated sources

PlatformIO runs these pre/post scripts:

- `scripts/patch_wolfssl.py`
- `scripts/build_html.py` (walks `src/`; generates adjacent headers from Web `.html` and `.js` inputs)
- `scripts/gen_i18n.py`
- `scripts/gen_builtin_cjk_font.py`
- `scripts/git_branch.py`
- `scripts/patch_jpegdec.py`
- `scripts/register_unit_tests_target.py`
- `scripts/register_safe_upload.py`

Do not hand-edit generated outputs:

- `src/**/*.generated.h`: edit the adjacent Web `.html` or `.js` input.
- `lib/I18n/I18nKeys.h`, `I18nStrings.h`, `I18nStrings.cpp`: edit `lib/I18n/translations/*.yaml` or the generator.
- `lib/GfxRenderer/cjk_ui_font_*.h`: edit the tracked font inputs/generation scripts and regenerate.
- `lib/Epub/Epub/hyphenation/generated/*`: change the source/generator path.

The active PlatformIO environment filters translation tables and the CJK glyph corpus. An i18n change that builds only `default` is insufficient release verification: build both SC and TC release environments and check generated glyph coverage. Those environment-specific builds rewrite tracked CJK headers, so finish by regenerating/restoring the canonical all-shipping-language headers (for example with `python3 scripts/gen_builtin_cjk_font.py`) and run `python3 scripts/check_cjk_ui_font_charset.py` before committing.

## Fork preservation contract

Read `docs/fork-features.md` before an upstream merge or a refactor in a high-risk subsystem. Preserve behavior, not merely symbol names.

### Fonts and catalog

- Reader SD font and UI SD font are independent persisted selections and runtime roles.
- Official UI packages contain physical 8, 10, and 12 pt faces. Load UI faces 12 -> 10 -> 8; incomplete manually installed families may use the nearest size within the same family.
- Keep locale-aware Noto Sans identities distinct; do not conflate SC, TC, JP, Serif, or arbitrary variants through fuzzy name matching.
- Rendering-affecting font unload/reload and registry updates follow the established render lock and cache-release ordering.
- `.cpfont` and `.cpfontpkg` installation is transactional. Preserve staged files, validation, rollback, legacy `.bak` recovery, cancellation, and the last usable family.
- Before TLS-heavy catalog work, release manifest family storage, glyph/renderer caches, and restorable resident SD fonts in the established order. Restore them only at the intended lifecycle points.
- Keep manifest and payload processing streaming and bounded. `HttpDownloader::maxBytes` is a generic upper bound, not an exact content-length assertion.
- Wi-Fi-selection return restores only the UI font; it must not mutate the reader-font selection.

### CJK, display, and input

- EN, Simplified Chinese, Traditional Chinese, and Japanese workflows must retain complete labels and required built-in glyphs.
- Missing built-in CJK UI glyphs may use a compatible SD-font fallback without unexpectedly changing Latin UI metrics.
- CJK line breaking, punctuation attachment, justification, indentation, actual installed reader sizes, and batched/metrics-only SD glyph access are intentional fork behavior.
- Dark-mode visible updates use `DarkRedrive` or its window equivalent. Do not replace them with `HALF_REFRESH` or ordinary `FAST_REFRESH` unless a documented hardware reason is verified on-device.
- Images and covers preserve their intended polarity in dark mode; UI background inversion must not double-invert image data.
- Reader orientation and full-UI Portrait/Portrait Inverted orientation are separate persisted settings. Input mapping follows the orientation actually drawn.
- Use logical mapped buttons in activities, not raw GPIO indices, except where the remapping implementation itself requires physical indices.

### Responsiveness, Web, and storage

- Home must become interactive before expensive cover extraction. Cover generation remains deferred, one item at a time, cancellable, cache-aware, and protected against stale render commits.
- Show indexing/progress UI before blocking reader work, and keep main-loop input polling ahead of blocking rendering, networking, display waits, and SD operations.
- Long rendering, indexing, SD, Web, WebDAV, download, and upload loops remain watchdog-safe and cancellation-aware.
- Deferred Web response callbacks must own durable state; never capture handler-local buffers by reference.
- Uploads and firmware writes stream with bounded memory and do not replace a valid destination on timeout, cancellation, invalid input, or partial write.
- Shared SD operations use `HalStorage` and existing locking/serialization. Do not introduce long-lived owning file handles in each SD font.
- New Wi-Fi scans clear stale scan state. Preserve bounded recovery, hidden-SSID entry, the pre-association `CN` country seed, and 802.11d so channels 12/13 remain discoverable.

### Persistence and recovery

Treat persisted JSON keys, enum values/order, defaults, negative SD font IDs, and migrations as data-format APIs. Reader/UI orientation, reader/UI font roles, sleep settings, and button mappings must remain backward-compatible.

Recovery and firmware update changes must validate image target, size, chip/device, and partition compatibility before writing. Preserve the fixed partition/dual-OTA recovery contract.
Preserve boot-button entry to the SD firmware picker. Flush deferred reader progress/crash-recovery state before deep sleep, and clear stale Calibre or other session-derived administrative values when their owning session ends.

Do not remove transaction, rollback, cancellation, generation-token, storage-locking, TLS teardown, dark-refresh, or migration code simply because it looks redundant. Much of it exists because a simpler path failed on real hardware.

## Embedded coding rules

- Exceptions are disabled; check every allocation and fallible operation.
- The ESP32-C3 has no PSRAM. Avoid file-sized buffers, unbounded data structures, and repeated allocation in render/directory/network loops.
- The approximately 48-52 KiB framebuffer is required display memory, not evidence of a leak.
- Keep stack allocations modest; use checked, bounded heap allocation for large temporary buffers and release them promptly.
- Use `memcpy` for potentially unaligned multi-byte serialized reads.
- Use `LOG_DBG`, `LOG_INF`, and `LOG_ERR`. Do not leave routine heap/timing telemetry enabled in production paths.
- Use i18n keys for user-visible text.
- Stop FreeRTOS tasks and release callbacks, files, fonts, and buffers before an activity is destroyed.
- Follow existing singleton/HAL patterns (`SETTINGS`, `APP_STATE`, `GUI`, `Storage`, `I18N`) rather than introducing an isolated architecture rewrite.
- Respect `.clang-format`; do not manually format around it.

## Verification

Minimum broad verification:

```bash
./bin/clang-format-fix -g  # use the all-files form only in an otherwise clean checkout
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
python3 scripts/check_cjk_ui_font_charset.py
python3 scripts/check_audit_security_contracts.py
python3 scripts/validate_release_contracts.py
pio run -e default -e sticky
git diff --check
git status --short
```

Use focused host-side runners from `test/` for changed behavior; examples include:

```bash
python3 test/run_i18n_translation_test.py
python3 test/run_cpfont_validator_test.py
python3 test/run_font_manifest_streaming_test.py
python3 test/run_font_catalog_ui_font_lifecycle_test.py
python3 test/run_home_existing_cover_reuse_test.py
python3 test/run_dark_mode_cover_background_test.py
./test/run_hyphenation_eval.sh <language>
```

Hardware-dependent display refresh, heap fragmentation/TLS, Wi-Fi scanning, SD timing, buttons/orientation, sleep, and OTA/recovery behavior require physical-device verification. Report exactly what was run; never claim device verification from a host build alone.

Before finishing, confirm that no `.pio/`, `platformio.local.ini`, credentials, serial logs, temporary scripts, or screenshots containing real SSIDs/MAC addresses are staged.
