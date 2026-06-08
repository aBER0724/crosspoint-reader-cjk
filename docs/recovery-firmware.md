# SD Recovery Firmware

SD Recovery is a special recovery firmware for devices that can still use `Install firmware from SD`, but cannot update through the normal OTA path.

Download the recovery `firmware.bin` from the [SD Recovery release](https://github.com/aBER0724/crosspoint-reader-cjk/releases/tag/sd-recovery). Put it on the SD card together with the target firmware you want to install.

The SD card root should contain only these two firmware files:

```text
SD card root/
├── firmware.bin         # Recovery firmware
└── firmware-target.bin  # Target firmware
```

The target firmware can use any name that starts with `firmware-` or `firmware_` and ends with `.bin`, for example:

```text
firmware-target.bin
firmware-v1.3.0.bin
firmware_1.3.0.bin
```

Do not name the target firmware `firmware.bin`. That name is reserved for the recovery firmware itself.

The release page has the user-facing recovery steps and safety notes. Maintainers can rebuild the recovery firmware with:

```bash
pio run -e recovery
```

The GitHub Actions workflow `.github/workflows/sd_recovery.yml` builds the recovery environment and publishes the single `firmware.bin` asset for the `sd-recovery` tag.
