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
        '"koPassword", StrId::STR_KOREADER_SYNC, true',
        failures,
        "KOReader password must be marked secret",
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


def check_web_auth_contract(failures: list[str]) -> None:
    cpp = read("src/network/CrossPointWebServer.cpp")
    gated_handlers = [
        "CrossPointWebServer::handleFileListData",
        "CrossPointWebServer::handleDownload",
        "CrossPointWebServer::handleUploadPost",
        "CrossPointWebServer::handleCreateFolder",
        "CrossPointWebServer::handleRename",
        "CrossPointWebServer::handleMove",
        "CrossPointWebServer::handleDelete",
        "CrossPointWebServer::handleGetSettings",
        "CrossPointWebServer::handlePostSettings",
        "CrossPointWebServer::handleGetOpdsServers",
        "CrossPointWebServer::handlePostOpdsServer",
        "CrossPointWebServer::handleDeleteOpdsServer",
        "CrossPointWebServer::handleWifiScan",
        "CrossPointWebServer::handleWifiSave",
        "CrossPointWebServer::handleWifiList",
        "CrossPointWebServer::handleWifiDelete",
    ]
    for handler in gated_handlers:
        require_function_contains(cpp, handler, "requireAdminAuth()", failures, "must require admin auth")

    require_function_contains(
        cpp,
        "CrossPointWebServer::handleUpload",
        "isAdminAuthorized()",
        failures,
        "multipart upload callback must reject unauthorized file bodies",
    )
    require_contains(
        "src/network/WebAdminAuth.cpp",
        "X-CrossPoint-Token",
        failures,
        "shared web auth policy must check the admin token header",
    )
    require_contains(
        "src/network/CrossPointWebServer.cpp",
        'START:<token>:<filename>:<size>:<path>',
        failures,
        "WebSocket protocol comment must require token",
    )
    require_contains(
        "src/network/CrossPointWebServer.cpp",
        "ERROR:Unauthorized",
        failures,
        "WebSocket upload must reject bad tokens",
    )

    dav = read("src/network/WebDAVHandler.cpp")
    require_function_contains(dav, "WebDAVHandler::handle", "isAuthorized(server)", failures, "must require auth")
    require_function_contains(dav, "WebDAVHandler::raw", "isAuthorized(server)", failures, "raw PUT must require auth")


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
    require_absent(r"\bsetInsecure\s*\(", ["src", "lib"], failures, "must not disable TLS verification")
    require_contains(
        "src/JsonSettingsIO.cpp",
        "writeFileAtomic",
        failures,
        "JSON settings persistence must use atomic helper",
    )
    require_contains(
        "src/JsonSettingsIO.cpp",
        "loadJsonWithBackup",
        failures,
        "JSON settings persistence must recover from backup/temp files",
    )
    require_contains(
        "src/JsonSettingsIO.cpp",
        "restoreJsonBackup",
        failures,
        "JSON settings persistence must restore backup after load failure",
    )
    require_contains(
        "src/CrossPointSettings.cpp",
        "loadSettingsFile",
        failures,
        "settings load path must use JSON backup recovery",
    )
    require_contains(
        "src/WifiCredentialStore.cpp",
        "loadWifiFile",
        failures,
        "Wi-Fi credential load path must use JSON backup recovery",
    )
    require_contains(
        "lib/KOReaderSync/KOReaderCredentialStore.cpp",
        "loadKOReaderFile",
        failures,
        "KOReader credential load path must use JSON backup recovery",
    )
    if "return Storage.writeFile(" in read("src/JsonSettingsIO.cpp"):
        failures.append("src/JsonSettingsIO.cpp: save paths must not directly return Storage.writeFile")


def check_frontend_contract(failures: list[str]) -> None:
    for path in [
        "src/network/html/HomePage.html",
        "src/network/html/SettingsPage.html",
        "src/network/html/FilesPage.html",
    ]:
        require_contains(path, "X-CrossPoint-Token", failures, "frontend requests must propagate admin token")
        require_contains(path, "withToken", failures, "frontend links/requests must carry admin token")
    require_contains(
        "src/network/html/FilesPage.html",
        "START:${adminToken}:",
        failures,
        "WebSocket upload START must include admin token",
    )
    require_contains(
        "src/network/html/FilesPage.html",
        "rewriteAuthedLinks",
        failures,
        "generated file links must be tokenized",
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
    checks = {
        "src/activities/reader/ReaderUtils.h": "displayBufferDarkRedrive",
        "src/activities/reader/XtcReaderActivity.cpp": "refreshContext.darkMode = wasDarkMode",
        "src/activities/util/ConfirmationActivity.cpp": "renderer.isDarkMode()",
        "src/activities/util/FullScreenMessageActivity.cpp": "renderer.isDarkMode()",
        "src/components/themes/BaseTheme.cpp": "renderer.isDarkMode()",
        "src/components/themes/lyra/LyraTheme.cpp": "renderer.isDarkMode()",
        "src/activities/boot_sleep/SleepActivity.cpp": "displaySleepBuffer()",
        "src/util/ScreenshotUtil.cpp": "renderer.isDarkMode()",
    }
    for path, needle in checks.items():
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
    check_web_auth_contract(failures)
    check_path_policy_contract(failures)
    check_tls_and_atomic_contracts(failures)
    check_frontend_contract(failures)
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
