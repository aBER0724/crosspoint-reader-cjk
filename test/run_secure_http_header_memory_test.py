#!/usr/bin/env python3
"""Lock the no-heap-growth HTTP response-header parsing contract."""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "freeink-sdk" / "libs" / "network" / "SecureNet" / "include" / "SecureHttpClient.h"
CLIENT_HEADER = ROOT / "freeink-sdk" / "libs" / "network" / "SecureNet" / "include" / "SecureClient.h"
CLIENT_SOURCE = ROOT / "freeink-sdk" / "libs" / "network" / "SecureNet" / "src" / "SecureClient.cpp"
DOWNLOADER_SOURCE = ROOT / "src" / "network" / "HttpDownloader.cpp"
FONT_DOWNLOAD_SOURCE = ROOT / "src" / "activities" / "settings" / "FontDownloadActivity.cpp"
PLATFORMIO = ROOT / "platformio.ini"


def main() -> int:
    content = SOURCE.read_text(encoding="utf-8")
    client_header = CLIENT_HEADER.read_text(encoding="utf-8")
    client_source = CLIENT_SOURCE.read_text(encoding="utf-8")
    downloader = DOWNLOADER_SOURCE.read_text(encoding="utf-8")
    font_download = FONT_DOWNLOAD_SOURCE.read_text(encoding="utf-8")
    platformio = PLATFORMIO.read_text(encoding="utf-8")
    required = [
        "char line[MAX_HEADER_LINE + 1];",
        "char line[MAX_CHUNK_LINE + 1];",
        "bool readLine(Client& c, char* line, const size_t capacity, unsigned long deadline,",
        "static constexpr size_t MAX_HEADER_LINE = 1024;",
        "static constexpr size_t MAX_CHUNK_LINE = 64;",
        "static constexpr size_t MAX_RESPONSE_HEADERS = 64;",
        "_location.reset();",
        "_location = std::move(location);",
        "std::unique_ptr<char[]> _location;",
    ]
    forbidden = [
        "std::vector<Header> _responseHeaders;",
        "_responseHeaders.push_back(Header{name, value});",
        "std::string line;\n      if (!readLine(*_conn, line, headerDeadline, shouldAbort))",
    ]

    failures = []
    required_contracts = [
        (platformio, "-DHAVE_MAX_FRAGMENT", "wolfSSL max_fragment_length support is not enabled"),
        (client_header, "void setTls12Only(bool enabled);", "SecureClient has no TLS 1.2-only policy setter"),
        (client_source, "if (_tls12Only)", "SecureClient does not honor the TLS 1.2-only policy"),
        (content, "void setTls12Only(bool enabled)", "SecureHttpClient has no TLS 1.2-only policy setter"),
        (content, "_secure.setTls12Only(_tls12Only);", "SecureHttpClient does not pass its TLS policy to SecureClient"),
        (downloader, 'constexpr const char* GITHUB_RELEASE_PREFIX = "https://github.com/";',
         "GitHub routing does not require the exact HTTPS origin"),
        (downloader, 'constexpr const char* RELEASE_DOWNLOAD_SEGMENT = "/releases/download/";',
         "GitHub routing does not require a release-download path"),
        (downloader, "bool isGitHubReleaseUrl(const std::string& url)",
         "HttpDownloader has no narrow GitHub Release URL recognizer"),
        (downloader, "const bool pinTls12 = isGitHubReleaseUrl(url);",
         "the TLS 1.2 decision is not made once for the whole GitHub Release chain"),
        (downloader, "http.setTls12Only(pinTls12);",
         "a hop is not pinned with the sticky (chain-wide) TLS 1.2 decision"),
        (downloader, "for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {",
         "redirect hops are not bounded by MAX_REDIRECTS"),
        (downloader, "freeink::SecureHttpClient http;",
         "no per-hop SecureHttpClient: one shared client would hold two live TLS \n         sessions and starve the X4 heap (handshake ok, then second-handshake\n         timeout at free heap 5040)"),
        (font_download, "constexpr size_t MAX_MANIFEST_BYTES = 64 * 1024;",
         "font manifest limit does not accommodate the live 41 KiB catalog"),
    ]
    for haystack, needle, message in required_contracts:
        if needle not in haystack:
            failures.append(message)
    for needle in required:
        if needle not in content:
            failures.append(f"missing required snippet: {needle}")
    for needle in forbidden:
        if needle in content:
            failures.append(f"forbidden allocation path remains: {needle}")

    if failures:
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1
    print("SECURE_HTTP_FIXED_HEADER_PARSING_OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
