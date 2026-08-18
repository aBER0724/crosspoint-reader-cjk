#!/usr/bin/env python3
"""Lock the no-heap-growth HTTP response-header parsing contract."""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "freeink-sdk" / "libs" / "network" / "SecureNet" / "include" / "SecureHttpClient.h"


def main() -> int:
    content = SOURCE.read_text(encoding="utf-8")
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
