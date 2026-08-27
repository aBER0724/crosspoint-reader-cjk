#!/usr/bin/env python3
"""Upload one firmware image to both OTA application slots.

This intentionally preserves the bootloader, partition table, OTA selection data,
NVS, SPIFFS, SD-card data, and coredump partition. Both application slots receive
the same image so the firmware boots regardless of which slot the preserved OTA
metadata currently selects. Changing the partition layout requires an explicit
recovery/full-flash procedure instead.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

APP0_OFFSET = 0x10000
APP1_OFFSET = 0x690000
APP_SLOT_SIZE = 0x680000


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=921600)
    parser.add_argument("--firmware", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    firmware = args.firmware.resolve()
    if not firmware.is_file():
        print(f"Firmware image not found: {firmware}", file=sys.stderr)
        return 2

    image_size = firmware.stat().st_size
    if image_size > APP_SLOT_SIZE:
        print(
            f"Firmware is too large for an OTA slot: {image_size} > {APP_SLOT_SIZE} bytes",
            file=sys.stderr,
        )
        return 2

    command = [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        "esp32c3",
        "--port",
        args.port,
        "--baud",
        str(args.baud),
        "write-flash",
        str(APP0_OFFSET),
        str(firmware),
        str(APP1_OFFSET),
        str(firmware),
    ]
    print(
        f"Safe OTA upload: {firmware} ({image_size} bytes) -> "
        f"app0 0x{APP0_OFFSET:x}, app1 0x{APP1_OFFSET:x}"
    )
    print("Preserving bootloader, partitions, otadata, NVS, SPIFFS, and coredump")
    return subprocess.run(command, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
