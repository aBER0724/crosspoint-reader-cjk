"""Register the routine factory-application upload command with PlatformIO."""

from pathlib import Path

Import("env")

project_dir = Path(env.subst("$PROJECT_DIR"))
uploader = project_dir / "scripts" / "upload_ota_slots.py"

# Xteink X3/X4 use a single factory application at 0x10000. Other targets
# (for example Sticky/ESP32-S3) retain the platform's normal uploader.
if env.BoardConfig().get("build.mcu", "") == "esp32c3":
    # Routine updates rewrite only the factory application. Recovery/layout
    # changes still require an explicit erase followed by a full upload.
    env.Replace(
        UPLOADCMD=(
            f'"$PYTHONEXE" "{uploader}" --port "$UPLOAD_PORT" '
            '--baud "$UPLOAD_SPEED" --firmware "$BUILD_DIR/${PROGNAME}.bin"'
        )
    )
