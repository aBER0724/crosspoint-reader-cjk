# AGENTS.md

Guidance for autonomous coding agents working in `crosspoint-reader-cjk`.
This repository is a CJK-focused fork of CrossPoint Reader. A successful upstream merge must preserve the fork's user-visible behavior, not merely compile.

## 1. Project and hardware

- Firmware: PlatformIO + Arduino, primarily C++20 (`-std=gnu++2a`) with Python build/test utilities.
- Main target: Xteink X4/X3, ESP32-C3, 16 MB flash, no PSRAM.
- Additional CI target: Seeed Studio XIAO ePaper Display Board / "Sticky", ESP32-S3.
- Main entry point: `src/main.cpp`; recovery entry point: `src/recovery/RecoveryMain.cpp`.
- The C3 is memory-constrained. The display buffer is required baseline memory, not a leak. Avoid file-sized buffers, repeated hot-path allocation, and unbounded containers.
- Exceptions are disabled. Allocation and I/O failures must be handled explicitly.

## 2. Clone and submodules

Clone recursively:

```bash
git clone --recursive <repo-url>
cd crosspoint-reader-cjk
git submodule update --init --recursive
```

Required submodules:

- `freeink-sdk/`: mandatory for all firmware builds. `platformio.ini` consumes SDK libraries through `symlink://freeink-sdk/...`, including display, input, storage, power, board configuration, UI, icons, and secure networking.
- `freeink-sdk/libs/assets/Icons/lucide`: nested source asset submodule used when regenerating Lucide-derived SDK icons. It is not needed to compile already-generated icon headers, but recursive initialization keeps the SDK checkout complete and matches CI.
`.gitmodules` names this fork's SDK update branch; the repository gitlink pins the reproducible SDK commit. Do not replace that gitlink with upstream SDK HEAD or edit the SDK incidentally. If an SDK change is required, verify the API in the checked-out submodule, commit it in the SDK repository first, and then update this repository's gitlink in a separate, intentional change.

Quick verification:

```bash
git submodule status --recursive
```

A leading `-` means the submodule is not initialized. A leading `+` means the checkout differs from the recorded gitlink.

## 3. Tooling and first build

Required tools:

- Python 3 (CI uses 3.14)
- PlatformIO Core (`pio`)
- clang-format 21+
- Pillow for built-in CJK font generation

The first build is slower because `custom_sdkconfig` rebuilds Arduino/ESP-IDF components. On macOS, retain any machine-specific CMake/toolchain workaround in gitignored `platformio.local.ini`; never commit that file.

Enable the repository's local Git hooks once per checkout:

```bash
./bin/install-git-hooks
```

The pre-commit hook formats tracked C/C++ changes, and the pre-push hook runs the
same cppcheck command as CI. A push is blocked when cppcheck reports any low, medium,
or high defect. For an exceptional one-off push when PlatformIO cannot run, explicitly
use `CROSSPOINT_SKIP_PRE_PUSH_CHECKS=1 git push`; CI still remains authoritative.

If an interrupted core rebuild reports multiple definitions of `app_main`, follow the cleanup command documented in `platformio.ini`. Do **not** use `git clean -fdX`, because it can delete local configuration and other intentionally ignored assets.

## 4. PlatformIO environments

Use the environment that matches the artifact or test:

| Environment | Purpose | Languages / notes |
|---|---|---|
| `default` | Normal ESP32-C3 development build | EN, Simplified Chinese, Traditional Chinese, Japanese; debug serial logging |
| `gh_release` | Simplified Chinese release | EN, SC, JA |
| `gh_release_rc` | Simplified Chinese release candidate | Inherits the SC release language set |
| `gh_release_tc` | Traditional Chinese release | EN, TC, JA |
| `gh_release_rc_tc` | Traditional Chinese release candidate | Inherits the TC release language set |
| `slim` | Size-focused C3 build | All four shipping languages; serial logging disabled |
| `device_test` | Physical-device automation | Test-only serial input injection; never publish this artifact |
| `recovery` | Minimal SD recovery firmware | English-only, separate source filter |
| `sticky` | ESP32-S3 Sticky development/CI build | Different MCU/toolchain; not interchangeable with X4 firmware |

Common commands:

```bash
pio run -e default
pio run -e sticky
pio run -e gh_release
pio run -e gh_release_tc
pio run -e recovery
pio run -e default --target upload
pio device monitor
```

The registered upload target is partition-aware for Xteink C3 environments. It writes application partitions and updates OTA selection while preserving the bootloader, partition table, and data partitions such as NVS, SPIFFS, and coredump; it does not preserve old application images in factory/OTA slots. Do not bypass `scripts/register_safe_upload.py` / `scripts/upload_ota_slots.py` with an arbitrary whole-flash command unless the task explicitly requires and validates the complete partition layout.

After physical tests with `device_test` or screenshot-only configurations, remove temporary overrides and restore a `default` development firmware to the device.

## 5. Build-time scripts and generated files

`platformio.ini` wires these scripts into every applicable build:

- `scripts/patch_wolfssl.py`: applies/checks the constrained-heap wolfSSL integration.
- `scripts/build_html.py`: walks `src/` and generates adjacent `*.generated.h` headers from Web `*.html` and `*.js` inputs, including nested `src/network/html/` directories.
- `scripts/gen_i18n.py`: generates `lib/I18n/I18nKeys.h`, `I18nStrings.h`, and `I18nStrings.cpp` from `lib/I18n/translations/*.yaml`, filtered by the environment's `custom_i18n_languages`.
- `scripts/gen_builtin_cjk_font.py`: generates size/weight-matched CJK UI font headers from the shipping translation corpus and the tracked source fonts in `fonts/`.
- `scripts/git_branch.py`: supplies development version metadata.
- `scripts/patch_jpegdec.py`: verifies/applies the JPEG decoder safety patch.
- `scripts/register_unit_tests_target.py`: registers the PlatformIO host/unit-test target.
- `scripts/register_safe_upload.py`: registers safe application upload behavior.

Generated/derived areas must not be edited as source:

- `src/**/*.generated.h` produced beside HTML/JavaScript Web inputs
- `lib/I18n/I18nKeys.h`
- `lib/I18n/I18nStrings.h`
- `lib/I18n/I18nStrings.cpp`
- `lib/GfxRenderer/cjk_ui_font_*.h`
- `lib/Epub/Epub/hyphenation/generated/*`

Edit the corresponding HTML, translation YAML, font-generation inputs/scripts, or hyphenation source and regenerate. Some generated outputs are intentionally tracked while others are ignored; follow `git status` and existing repository policy rather than assuming all generated files should be committed.

For i18n/CJK changes, remember that the selected PlatformIO environment changes both the generated language tables and built-in glyph corpus. Verify both SC and TC release environments so one locale is not accidentally omitted. Environment-specific builds rewrite the tracked CJK headers; after those builds, regenerate/restore the canonical all-shipping-language headers (for example with `python3 scripts/gen_builtin_cjk_font.py`) and run `python3 scripts/check_cjk_ui_font_charset.py` before committing.

## 6. Verification commands

Baseline before a PR:

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

CI builds `default` and `sticky` in one PlatformIO invocation. Preserve this pattern when reproducing CI because separate invocations can invalidate or remove environment build artifacts after the second toolchain is installed.

Tests are primarily focused host-side runners rather than a single conventional `pio test` suite. Run the narrow `test/run_*.py` or shell runner for the subsystem touched. Important examples:

```bash
./test/run_hyphenation_eval.sh
./test/run_hyphenation_eval.sh english
python3 test/run_i18n_translation_test.py
python3 test/run_cpfont_validator_test.py
python3 test/run_font_manifest_streaming_test.py
python3 test/run_home_existing_cover_reuse_test.py
python3 test/run_dark_mode_cover_background_test.py
```

When font catalog, UI font lifecycle, low-memory networking, SD font caching, Web uploads, persistence, display refresh, orientation, recovery, or hardware input changes, run the matching focused tests listed in `test/` and perform physical-device verification. Do not dismiss an existing test failure without determining whether it is pre-existing and documenting the evidence.

## 7. C++ style and embedded constraints

- `.clang-format` is authoritative: 2 spaces, 120-column limit, attached braces, `Type* ptr` pointer alignment.
- Include what you use. Prefer matching header, platform/external headers, standard headers, then other project headers.
- Classes/types: `PascalCase`; functions/variables: `camelCase`; macros and most constants: `UPPER_SNAKE_CASE`.
- Use fixed-width integers for persisted/protocol fields and `size_t` for sizes/indices.
- Prefer `const` correctness, references, and `std::unique_ptr`; avoid `std::shared_ptr` unless ownership truly requires it.
- Use `std::string` internally and Arduino `String` at Arduino/WebServer boundaries, following nearby code. Avoid allocation-heavy string construction in render and directory loops.
- Prefer early returns. Check allocation, SD, network, and parser results immediately.
- Use `LOG_DBG`, `LOG_INF`, and `LOG_ERR`; do not add routine periodic telemetry to normal builds.
- After `strncpy`, force null termination.
- Never dereference potentially unaligned serialized data through a wider pointer; use `memcpy`.
- Keep long loops watchdog-safe and cancellation-aware. Yield/feed the watchdog where existing policy requires it.
- Use `HalStorage` and repository locking conventions. Do not keep owning SD file handles resident in each font object or access the card concurrently outside the established serialization paths.
- Activities are heap allocated and destroyed on transition. Stop tasks and release files, buffers, fonts, and callbacks before activity destruction.
- User-facing strings must use i18n keys; logging text may be literal.
- Follow existing singleton access (`SETTINGS`, `APP_STATE`, `GUI`, `Storage`, `I18N`) rather than introducing isolated dependency-injection architecture.

## 8. Fork behavior that upstream merges must preserve

`docs/fork-features.md` is the detailed behavior contract and post-merge checklist. Read it before resolving upstream conflicts in high-risk paths. At minimum preserve these invariants:

### CJK i18n and releases

- EN/SC/TC/JA workflows remain complete, including settings, Wi-Fi, font catalog, errors, updates, and recovery-facing labels.
- Built-in CJK UI glyphs come from the selected shipping translations and required UI symbols.
- Missing built-in CJK UI glyphs may fall back to a compatible SD font without unexpectedly changing Latin UI metrics.
- Build both `gh_release` and `gh_release_tc`; they intentionally have different language sets and version suffixes.

### Independent reader and UI fonts

- Reader and UI SD font selections are separate persisted settings and separate runtime roles.
- UI packages require physical 8/10/12 pt faces for official packages. Runtime fallback may reuse the nearest installed size within the same family when a manually installed family is incomplete.
- UI faces load largest-first (12, 10, 8) to make low-memory fallback deterministic.
- Do not merge Noto Sans identities by name substring in a way that conflates SC, TC, JP, Serif, or unrelated variants.
- Font unload/reload and registry changes that affect rendering must hold the established render lock.

### Font catalog and network memory

- `.cpfont`/`.cpfontpkg` install and replacement are staged transactions. Failure, cancellation, restart, or a stale `.bak` must not destroy the last usable family.
- Validate byte bounds, SHA-256, CPFont structure/version, range responses, and final paths before activation.
- Release manifest families, glyph/renderer caches, and resident SD fonts before TLS-heavy transfers; restore only at the lifecycle points established by the catalog.
- Keep streaming/range downloads bounded. `HttpDownloader::maxBytes` is an upper bound, not an exact expected object length.
- Wi-Fi selector restoration is UI-font-only; it must not unexpectedly replace the reader font.

### Display, orientation, and Home

- In dark mode, visible updates use `DarkRedrive` (buffer or window variant). Do not fall through to `HALF_REFRESH` or ordinary `FAST_REFRESH` without a documented, device-tested hardware reason.
- Reader images/covers preserve their polarity while surrounding dark UI fills invert appropriately.
- Reader orientation and full-UI Portrait/Portrait Inverted orientation are independent persisted settings. Input mapping follows the orientation actually rendered.
- Home cover extraction/conversion is deferred, one item at a time, cancellable, and guarded against stale renders. Do not move expensive EPUB/XTC cover generation back into initial draw or input handling.
- Show indexing/progress UI before blocking reader work, and keep main-loop input polling ahead of blocking rendering, networking, display waits, and SD operations.

### Web, Wi-Fi, storage, and persistence

- Long Web/WebDAV/upload responses use bounded streaming, durable callback state, watchdog-safe writes, and rollback for partial uploads.
- A new Wi-Fi scan clears stale Arduino scan state. Recovery is bounded, hidden SSID entry remains available, and the pre-association `CN` seed plus 802.11d behavior preserves channels 12/13 hotspot discovery.
- Persisted JSON keys, enum numeric values/order, defaults, negative SD font IDs, and migration behavior are compatibility APIs.
- Firmware/recovery writes validate target and partition constraints before writing and preserve the dual-OTA recovery contract.
- Preserve boot-button entry to the SD firmware picker, flush deferred reader progress/crash-recovery state before deep sleep, and clear stale Calibre/session-derived administrative values when their session ends.

Do not "simplify" transaction, rollback, cancellation, render-generation, TLS teardown, storage-locking, dark-refresh, or migration logic merely because it appears repetitive. These paths encode fixes for real device failures.

## 9. Upstream and Git workflow

Before editing or merging:

```bash
git branch --show-current
git remote -v
git status --short
git submodule status --recursive
```

- Preserve unrelated user modifications and local ignored assets.
- Do not assume `origin` is upstream or that the primary branch is named `main`.
- Review upstream merges by behavior and by the high-risk path table in `docs/fork-features.md`, not only by conflict count.
- Keep main-repository and `freeink-sdk` commits separate where practical.
- Never commit `.pio/`, `platformio.local.ini`, credentials, device screenshots containing SSIDs/MAC addresses, or temporary diagnostic assets.
- Use focused semantic commits and list exact verification performed.
- Do not rewrite shared history or force-push unless the user explicitly authorizes that destructive operation; use explicit `--force-with-lease` when authorized.

## 10. High-risk paths

Consult `docs/fork-features.md` for the maintained table. Common conflict hotspots include:

- `platformio.ini`, `partitions.csv`, `scripts/*`, and the `freeink-sdk` gitlink
- `lib/I18n/*`, `lib/GfxRenderer/*`, `lib/EpdFont/*`, `lib/ExternalFont/*`, `lib/ReaderRuntime/*`
- `src/CrossPointSettings.*`, `src/SettingsList.h`, `src/SdCardFontSystem.*`
- Home, reader, Wi-Fi, font settings/catalog, firmware update, and recovery activities
- `src/network/CrossPointWebServer.*`, `HttpDownloader.*`, WebDAV/upload code
- `lib/hal/HalStorage.*` and SD/cache serialization paths

For any change in these areas, read nearby code, retain existing guard order, run focused contract tests, build relevant environments, and verify on hardware when behavior depends on e-paper refresh, heap fragmentation, Wi-Fi/TLS, SD timing, buttons, sleep, or OTA partitions.
