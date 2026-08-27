"""Register the partition-aware application-only upload command."""

from pathlib import Path

Import("env")

project_dir = Path(env.subst("$PROJECT_DIR"))
uploader = project_dir / "scripts" / "upload_ota_slots.py"

# Xteink X3/X4 devices may contain either the legacy factory layout or the
# fixed dual-OTA layout. Other targets retain the platform's normal uploader.
if env.BoardConfig().get("build.mcu", "") == "esp32c3":
    # Read the installed layout, update every application partition, and select
    # the first OTA slot explicitly. Bootloader, layout, and user data are preserved.
    env.Replace(
        UPLOADCMD=(
            f'"$PYTHONEXE" "{uploader}" --port "$UPLOAD_PORT" '
            '--baud "$UPLOAD_SPEED" --firmware "$BUILD_DIR/${PROGNAME}.bin"'
        )
    )
