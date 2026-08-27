#!/usr/bin/env python3
"""Upload firmware to every application partition in the device's current layout.

The partition table is read from the device first, so routine uploads remain safe
across factory and dual-OTA layouts. Bootloader, partition table, OTA selection data,
NVS, SPIFFS, SD-card data, and coredump are preserved.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

PARTITION_TABLE_OFFSET = 0x8000
PARTITION_TABLE_SIZE = 0x1000
PARTITION_ENTRY_SIZE = 32
PARTITION_MAGIC = 0x50AA
PARTITION_TYPE_APP = 0x00
PARTITION_SUBTYPE_FACTORY = 0x00
PARTITION_SUBTYPE_OTA_MIN = 0x10
PARTITION_SUBTYPE_OTA_MAX = 0x1F
OTADATA_PARTITION_SIZE = 0x2000
OTA_SELECT_ENTRY_SIZE = 32
OTA_SELECT_SECTOR_SIZE = 0x1000


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=921600)
    parser.add_argument("--firmware", required=True, type=Path)
    return parser.parse_args()


def read_partition_table(port: str, baud: int) -> bytes:
    with tempfile.NamedTemporaryFile() as partition_file:
        command = [
            sys.executable,
            "-m",
            "esptool",
            "--chip",
            "esp32c3",
            "--port",
            port,
            "--baud",
            str(baud),
            "read-flash",
            hex(PARTITION_TABLE_OFFSET),
            hex(PARTITION_TABLE_SIZE),
            partition_file.name,
        ]
        subprocess.run(command, check=True)
        partition_file.seek(0)
        return partition_file.read()


def parse_partition_table(table: bytes) -> tuple[list[tuple[str, int, int, int]], tuple[int, int] | None]:
    partitions: list[tuple[str, int, int, int]] = []
    otadata: tuple[int, int] | None = None
    for pos in range(0, len(table) - PARTITION_ENTRY_SIZE + 1, PARTITION_ENTRY_SIZE):
        entry = table[pos : pos + PARTITION_ENTRY_SIZE]
        magic, part_type, subtype, offset, size = struct.unpack_from("<HBBII", entry)
        if magic != PARTITION_MAGIC:
            break
        if part_type == 0x01 and subtype == 0x00:
            otadata = (offset, size)
            continue
        if part_type != PARTITION_TYPE_APP:
            continue
        is_factory = subtype == PARTITION_SUBTYPE_FACTORY
        is_ota = PARTITION_SUBTYPE_OTA_MIN <= subtype <= PARTITION_SUBTYPE_OTA_MAX
        if not is_factory and not is_ota:
            continue
        label = entry[12:28].split(b"\0", 1)[0].decode("ascii", "replace")
        partitions.append((label or f"app_{subtype:02x}", subtype, offset, size))
    return partitions, otadata


def build_otadata(ota_slot: int) -> bytes:
    sequence = ota_slot + 1
    entry = bytearray(b"\xff" * OTA_SELECT_ENTRY_SIZE)
    struct.pack_into("<I", entry, 0, sequence)
    struct.pack_into("<I", entry, 28, zlib.crc32(entry[:4], 0xFFFFFFFF) & 0xFFFFFFFF)
    data = bytearray(b"\xff" * OTADATA_PARTITION_SIZE)
    data[:OTA_SELECT_ENTRY_SIZE] = entry
    data[OTA_SELECT_SECTOR_SIZE : OTA_SELECT_SECTOR_SIZE + OTA_SELECT_ENTRY_SIZE] = entry
    return bytes(data)


def main() -> int:
    args = parse_args()
    firmware = args.firmware.resolve()
    if not firmware.is_file():
        print(f"Firmware image not found: {firmware}", file=sys.stderr)
        return 2

    try:
        partitions, otadata = parse_partition_table(read_partition_table(args.port, args.baud))
    except subprocess.CalledProcessError as exc:
        print(f"Failed to read the device partition table: {exc}", file=sys.stderr)
        return exc.returncode or 1

    if not partitions:
        print("No application partitions found in the device partition table", file=sys.stderr)
        return 2

    image_size = firmware.stat().st_size
    undersized = [(label, size) for label, _, _, size in partitions if image_size > size]
    if undersized:
        details = ", ".join(f"{label}: {size}" for label, size in undersized)
        print(
            f"Firmware is too large ({image_size} bytes) for device partition(s): {details}",
            file=sys.stderr,
        )
        return 2

    with tempfile.NamedTemporaryFile() as otadata_file:
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
        ]
        for _, _, offset, _ in partitions:
            command.extend((hex(offset), str(firmware)))

        ota_partitions = [
            part
            for part in partitions
            if PARTITION_SUBTYPE_OTA_MIN <= part[1] <= PARTITION_SUBTYPE_OTA_MAX
        ]
        if ota_partitions:
            if otadata is None or otadata[1] < OTADATA_PARTITION_SIZE:
                print("Dual-OTA applications found without a valid otadata partition", file=sys.stderr)
                return 2
            boot_slot = ota_partitions[0][1] - PARTITION_SUBTYPE_OTA_MIN
            otadata_file.write(build_otadata(boot_slot))
            otadata_file.flush()
            command.extend((hex(otadata[0]), otadata_file.name))

        destinations = ", ".join(f"{label} 0x{offset:x}" for label, _, offset, _ in partitions)
        print(f"Safe application upload: {firmware} ({image_size} bytes) -> {destinations}")
        if ota_partitions:
            print(f"Selecting {ota_partitions[0][0]} as the next boot partition")
        print("Preserving bootloader, partition table, NVS, SPIFFS, and coredump")
        return subprocess.run(command, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
