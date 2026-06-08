#!/usr/bin/env python3
from pathlib import Path
import shutil
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DIST = ROOT / "dist"
PACKAGE_ROOT = DIST / "crosspoint-recovery-sd"
SD_ROOT = PACKAGE_ROOT / "SD-CARD-CONTENTS"

RECOVERY_BIN = ROOT / ".pio" / "build" / "recovery" / "firmware.bin"
SC_BIN = ROOT / ".pio" / "build" / "gh_release" / "firmware.bin"
TC_BIN = ROOT / ".pio" / "build" / "gh_release_tc" / "firmware.bin"

README_TEMPLATE = """CrossPoint Reader SD Recovery Package

For users without Python, PlatformIO, or command-line tools.

1. Copy everything inside SD-CARD-CONTENTS to the root of the SD card.
2. Insert the SD card into the reader.
3. On firmware 0.3.3, choose Install firmware from SD.
4. The device first installs firmware.bin, which is the recovery firmware.
5. Recovery starts automatically and installs {payload_name}.
6. Wait until the screen says the install is complete and the device restarts.
7. After the final firmware boots, remove firmware.bin and {payload_name} from the SD card.

Recovery treats files named firmware-* or firmware_* ending in .bin as target firmware.
It deliberately ignores firmware.bin because that file is the recovery firmware itself.

If recovery shows PAYLOAD NOT FOUND, copy {payload_name} to the SD card root again.
If recovery shows SD NOT READY, reinsert or reformat the SD card and try again.
"""


def require_file(path: Path) -> None:
    if not path.exists():
        raise SystemExit(f"Missing required file: {path}")


def reset_dir(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True)


def write_zip(zip_path: Path) -> None:
    if zip_path.exists():
        zip_path.unlink()

    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for file in PACKAGE_ROOT.rglob("*"):
            if file.is_file():
                zf.write(file, file.relative_to(PACKAGE_ROOT.parent))


def copy_package(payload: Path, payload_name: str, suffix: str) -> Path:
    require_file(RECOVERY_BIN)
    require_file(payload)

    reset_dir(SD_ROOT)
    shutil.copy2(RECOVERY_BIN, SD_ROOT / "firmware.bin")
    shutil.copy2(payload, SD_ROOT / payload_name)
    (PACKAGE_ROOT / "README.txt").write_text(README_TEMPLATE.format(payload_name=payload_name), encoding="utf-8")

    zip_path = DIST / f"crosspoint-recovery-sd-{suffix}.zip"
    write_zip(zip_path)
    return zip_path


def main() -> None:
    DIST.mkdir(exist_ok=True)

    sc_zip = copy_package(SC_BIN, "firmware-target.bin", "sc")
    print(f"Wrote {sc_zip}")

    tc_zip = copy_package(TC_BIN, "firmware-target.bin", "tc")
    print(f"Wrote {tc_zip}")


if __name__ == "__main__":
    main()
