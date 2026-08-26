#!/usr/bin/env python3
"""Upload one firmware image to the factory application partition.

This routine update preserves the bootloader, partition table, NVS, SPIFFS,
SD-card data, and coredump partition. Partition-layout changes require an
explicit erase followed by a full flash.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

FACTORY_OFFSET = 0x10000
FACTORY_SIZE = 0xD00000


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
    if image_size > FACTORY_SIZE:
        print(
            f"Firmware is too large for the factory partition: {image_size} > {FACTORY_SIZE} bytes",
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
        "--before",
        "default-reset",
        "--after",
        "hard-reset",
        "write-flash",
        "--flash-mode",
        "keep",
        "--flash-freq",
        "keep",
        "--flash-size",
        "keep",
        str(FACTORY_OFFSET),
        str(firmware),
    ]
    print(f"Safe factory upload: {firmware} ({image_size} bytes) -> factory 0x{FACTORY_OFFSET:x}")
    print("Preserving bootloader, partitions, NVS, SPIFFS, and coredump")
    return subprocess.run(command, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
