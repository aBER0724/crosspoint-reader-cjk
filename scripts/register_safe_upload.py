"""Register the routine application-only upload command with PlatformIO."""

from pathlib import Path

Import("env")

project_dir = Path(env.subst("$PROJECT_DIR"))
uploader = project_dir / "scripts" / "upload_ota_slots.py"

# The Xteink X3/X4 firmware uses the fixed C3 dual-OTA partition layout below.
# Other targets (for example Sticky/ESP32-S3) retain the platform's normal uploader.
if env.BoardConfig().get("build.mcu", "") == "esp32c3":
    # Routine updates must not rewrite bootloader, partition table, or OTA metadata.
    # Both slots receive the same image so either already-selected slot boots it.
    env.Replace(
        UPLOADCMD=(
            f'"$PYTHONEXE" "{uploader}" --port "$UPLOAD_PORT" '
            '--baud "$UPLOAD_SPEED" --firmware "$BUILD_DIR/${PROGNAME}.bin"'
        )
    )
