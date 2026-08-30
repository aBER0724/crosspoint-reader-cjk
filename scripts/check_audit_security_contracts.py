#!/usr/bin/env python3
"""Static regression checks for audit-remediation security contracts."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require_contains(path: str, needle: str, failures: list[str], message: str) -> None:
    if needle not in read(path):
        failures.append(f"{path}: {message}")


def require_absent(pattern: str, roots: list[str], failures: list[str], message: str) -> None:
    regex = re.compile(pattern)
    for root in roots:
        for path in (ROOT / root).rglob("*"):
            if not path.is_file() or path.suffix not in {".cpp", ".h", ".hpp", ".ino"}:
                continue
            rel = path.relative_to(ROOT)
            text = path.read_text(encoding="utf-8", errors="ignore")
            if regex.search(text):
                failures.append(f"{rel}: {message}")


def function_body(text: str, qualified_name: str) -> str:
    match = re.search(rf"\b{re.escape(qualified_name)}\s*\([^)]*\)\s*(?:const\s*)?\{{", text)
    if not match:
        return ""

    start = match.end() - 1
    depth = 0
    for index in range(start, len(text)):
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[start : index + 1]
    return ""


def require_function_contains(
    text: str, qualified_name: str, needle: str, failures: list[str], message: str
) -> None:
    body = function_body(text, qualified_name)
    if not body:
        failures.append(f"{qualified_name}: function not found")
    elif needle not in body:
        failures.append(f"{qualified_name}: {message}")


def check_secret_redaction(failures: list[str]) -> None:
    require_contains(
        "src/SettingsList.h",
        '"koPassword", StrId::STR_KOREADER_SYNC)',
        failures,
        "KOReader password setting must remain present",
    )
    require_contains(
        "src/SettingsList.h",
        ".withObfuscated()",
        failures,
        "KOReader password must be marked obfuscated/secret for web API",
    )
    require_contains(
        "src/network/CrossPointWebServer.cpp",
        'doc["secret"] = true;',
        failures,
        "settings API must emit secret metadata instead of secret values",
    )
    require_contains(
        "src/network/html/SettingsPage.html",
        "setting.secret",
        failures,
        "settings page must treat secret fields as write-only",
    )


def check_path_policy_contract(failures: list[str]) -> None:
    require_contains(
        "src/network/StoragePathPolicy.cpp",
        "bool isProtectedPath",
        failures,
        "shared storage path policy must exist",
    )
    cpp = read("src/network/CrossPointWebServer.cpp")
    for handler in [
        "CrossPointWebServer::handleFileListData",
        "CrossPointWebServer::handleDownload",
        "CrossPointWebServer::handleUpload",
        "CrossPointWebServer::handleCreateFolder",
        "CrossPointWebServer::handleRename",
        "CrossPointWebServer::handleMove",
        "CrossPointWebServer::handleDelete",
    ]:
        require_function_contains(cpp, handler, "isProtectedWebPath", failures, "must enforce protected paths")
    require_contains(
        "src/network/WebDAVHandler.cpp",
        "StoragePathPolicy::isProtectedPath",
        failures,
        "WebDAV must use shared protected path policy",
    )
    dav = read("src/network/WebDAVHandler.cpp")
    for handler in [
        "WebDAVHandler::handlePropfind",
        "WebDAVHandler::handleGet",
        "WebDAVHandler::handleHead",
        "WebDAVHandler::handlePut",
        "WebDAVHandler::handleDelete",
        "WebDAVHandler::handleMkcol",
        "WebDAVHandler::handleMove",
        "WebDAVHandler::handleCopy",
        "WebDAVHandler::handleLock",
        "WebDAVHandler::handleUnlock",
    ]:
        require_function_contains(dav, handler, "isProtectedPath", failures, "must enforce protected paths")


def check_tls_and_atomic_contracts(failures: list[str]) -> None:
    # Tip 1.5.0 FreeInk SecureHttpClient still uses setInsecure for KOReader and the
    # FREEINK_NET_WOLFSSL download path. Require the default ESP-TLS downloader to keep
    # CA-bundle verification, and only flag unexpected setInsecure call sites.
    require_contains(
        "src/network/HttpDownloader.cpp",
        "esp_crt_bundle_attach",
        failures,
        "default HTTPS downloader must attach the ESP CA bundle",
    )
    allowed_insecure = {
        Path("src/network/HttpDownloader.cpp"),
        Path("lib/KOReaderSync/KOReaderSyncClient.cpp"),
    }
    insecure_re = re.compile(r"\bsetInsecure\s*\(")
    for root in ["src", "lib"]:
        for path in (ROOT / root).rglob("*"):
            if not path.is_file() or path.suffix not in {".cpp", ".h", ".hpp", ".ino"}:
                continue
            rel = path.relative_to(ROOT)
            if rel in allowed_insecure:
                continue
            text = path.read_text(encoding="utf-8", errors="ignore")
            if insecure_re.search(text):
                failures.append(f"{rel}: must not disable TLS verification")
    # Tip 1.5.0 replaced JsonSettingsIO with PersistableStore CRTP.
    require_contains(
        "lib/Serialization/PersistableStore.cpp",
        "writeDocToFile",
        failures,
        "JSON settings persistence must serialize through PersistableStore",
    )
    require_contains(
        "lib/Serialization/PersistableStore.cpp",
        "readDocFromFile",
        failures,
        "JSON settings persistence must parse through PersistableStore",
    )
    require_contains(
        "lib/Serialization/PersistableStore.h",
        "saveToFile",
        failures,
        "PersistableStore must expose saveToFile under storeMutex",
    )
    require_contains(
        "lib/Serialization/PersistableStore.h",
        "loadFromFile",
        failures,
        "PersistableStore must expose loadFromFile under storeMutex",
    )
    require_contains(
        "src/CrossPointSettings.h",
        "public PersistableStore<CrossPointSettings>",
        failures,
        "settings store must use PersistableStore CRTP",
    )
    require_contains(
        "src/WifiCredentialStore.h",
        "public PersistableStore<WifiCredentialStore>",
        failures,
        "Wi-Fi credential store must use PersistableStore CRTP",
    )
    require_contains(
        "lib/KOReaderSync/KOReaderCredentialStore.h",
        "public PersistableStore<KOReaderCredentialStore>",
        failures,
        "KOReader credential store must use PersistableStore CRTP",
    )
    require_contains(
        "src/main.cpp",
        "SETTINGS.loadFromFile()",
        failures,
        "boot path must load settings through PersistableStore",
    )
    # PersistableStore centralizes Storage.writeFile; keep the explicit result check.
    if "return Storage.writeFile(" in read("lib/Serialization/PersistableStore.cpp"):
        failures.append(
            "lib/Serialization/PersistableStore.cpp: writeDocToFile must check Storage.writeFile result"
        )


def check_upload_resource_contract(failures: list[str]) -> None:
    require_contains(
        "src/network/CrossPointWebServer.cpp",
        "MAX_WEB_UPLOAD_BYTES",
        failures,
        "web uploads must have a size cap",
    )
    require_contains(
        "src/network/CrossPointWebServer.cpp",
        "MAX_WEB_UPLOAD_MS",
        failures,
        "web uploads must have a timeout",
    )
    require_contains(
        "src/network/CrossPointWebServer.cpp",
        "Upload exceeds maximum size",
        failures,
        "oversized uploads must be rejected",
    )
    require_contains(
        "src/network/CrossPointWebServer.cpp",
        "Upload timed out",
        failures,
        "timed out uploads must be rejected",
    )
    require_contains(
        "src/network/CrossPointWebServer.cpp",
        "MAX_WEB_LIST_ENTRIES",
        failures,
        "directory listings must have a bounded entry count",
    )
    require_contains(
        "src/network/CrossPointWebServer.cpp",
        "WEB_STORAGE_RESERVE_BYTES",
        failures,
        "web uploads must reserve SD space for device state",
    )
    require_contains(
        "src/network/CrossPointWebServer.cpp",
        "Storage.freeBytes()",
        failures,
        "web uploads must check SD free space",
    )
    require_contains(
        "lib/hal/HalStorage.h",
        "freeBytes",
        failures,
        "storage layer must expose free-space checks",
    )
    require_contains(
        "src/network/WebDAVHandler.cpp",
        "MAX_DAV_UPLOAD_BYTES",
        failures,
        "WebDAV uploads must have a size cap",
    )
    require_contains(
        "src/network/WebDAVHandler.cpp",
        "MAX_DAV_UPLOAD_MS",
        failures,
        "WebDAV uploads must have a timeout",
    )
    require_contains(
        "src/network/WebDAVHandler.cpp",
        "DAV_STORAGE_RESERVE_BYTES",
        failures,
        "WebDAV uploads must reserve SD space for device state",
    )

def check_dark_mode_contract(failures: list[str]) -> None:
    # Tip 1.5.0 + fork hybrid dark policy: high-traffic visible refreshes must either
    # call displayBufferDarkRedrive or branch on isDarkMode. Exact tip needles for
    # Xtc/Lyra no longer exist after activity-first ForcedRefresh / theme refactors.
    checks = [
        ("src/activities/reader/ReaderUtils.h", "displayBufferDarkRedrive"),
        ("src/activities/reader/XtcReaderActivity.cpp", "displayBufferDarkRedrive"),
        ("src/activities/util/ConfirmationActivity.cpp", "renderer.isDarkMode()"),
        ("src/activities/util/FullScreenMessageActivity.cpp", "renderer.isDarkMode()"),
        ("src/components/themes/BaseTheme.cpp", "renderer.isDarkMode()"),
        ("src/components/themes/BaseTheme.cpp", "displayBufferDarkRedrive"),
        ("src/activities/boot_sleep/SleepActivity.cpp", "displaySleepBuffer()"),
        ("src/util/ScreenshotUtil.cpp", "renderer.isDarkMode()"),
    ]
    for path, needle in checks:
        require_contains(path, needle, failures, "visible refresh path must branch for dark mode")
    require_contains(
        "src/activities/boot_sleep/SleepActivity.cpp",
        "renderer.displayBufferDarkRedrive()",
        failures,
        "sleep screen refresh must use dark redrive after dark-mode entry",
    )


def main() -> int:
    failures: list[str] = []
    check_secret_redaction(failures)
    check_path_policy_contract(failures)
    check_tls_and_atomic_contracts(failures)
    check_upload_resource_contract(failures)
    check_dark_mode_contract(failures)

    if failures:
        print("Audit security contract checks failed:")
        for failure in failures:
            print(f"- {failure}")
        return 1

    print("Audit security contract checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
