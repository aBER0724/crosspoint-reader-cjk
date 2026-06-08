# SD Recovery Firmware

This document describes the two-stage SD recovery package for users on firmware 0.3.3 whose OTA path is unreliable.

## User Flow

Users do not need Python, PlatformIO, esptool, or command-line tools.

1. Download the recovery ZIP.
2. Copy the contents of `SD-CARD-CONTENTS` to the SD card root.
3. Insert the SD card.
4. Use firmware 0.3.3's `Install firmware from SD` action.
5. The reader installs `firmware.bin`, which is the recovery firmware.
6. Recovery boots and installs the target firmware from `firmware-target.bin`.
7. Remove `firmware.bin` and `firmware-target.bin` after the final firmware boots.

## Why Two Stages

Firmware 0.3.3 already accepts `/firmware.bin` from SD, but its OTA path has high memory pressure. The recovery firmware is intentionally small and SD-first. It avoids EPUB parsing, library scanning, WebServer, GitHub JSON parsing, and HTTPS OTA.

## Build Commands

```bash
pio run -e recovery
pio run -e gh_release
pio run -e gh_release_tc
python3 scripts/build_recovery_package.py
```

The generated ZIP files are written under `dist/`:

- `dist/crosspoint-recovery-sd-sc.zip`
- `dist/crosspoint-recovery-sd-tc.zip`

## ZIP Layout

Simplified Chinese package:

```text
crosspoint-recovery-sd/
├── README.txt
└── SD-CARD-CONTENTS/
    ├── firmware.bin
    └── firmware-target.bin
```

Traditional Chinese package:

```text
crosspoint-recovery-sd/
├── README.txt
└── SD-CARD-CONTENTS/
    ├── firmware.bin
    └── firmware-target.bin
```

## Compatibility

The recovery firmware is an ESP32-C3 app image and must remain smaller than the app partition size, `0x680000` bytes. It uses the existing partition table and writes only through the normal OTA app partition flow.

Firmware 0.3.3 scans `/firmware-sc.bin`, `/firmware-tc.bin`, and `/firmware.bin` on the SD card root. The recovery package uses root `firmware.bin` as the first-stage image to keep the user instructions simple.

After recovery boots, it scans the SD root and `/recovery-payload` for target firmware files whose names start with `firmware-` or `firmware_` and end with `.bin`. It deliberately ignores root `/firmware.bin`, because that file is the recovery firmware itself.

## Memory Notes

Recovery avoids normal app subsystems and links a recovery-only entry point. It reuses `FirmwareInstaller`, which allocates one 4096-byte heap buffer during firmware writes and frees it immediately after use. The buffer is intentionally not placed on the stack because ESP32-C3 stack headroom is limited.

## Limits

This package cannot repair a damaged bootloader, damaged partition table, or a device whose 0.3.3 SD update entry cannot run. Those cases require ROM bootloader flashing or hardware service recovery.
