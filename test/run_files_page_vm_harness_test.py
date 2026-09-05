#!/usr/bin/env python3
"""Extracts the FilesPage script bundle and runs it in a Node vm harness.

The 1.5.0 upstream merge silently dropped JS-referenced DOM elements and
function/const declarations from this page, which crashed openUploadModal in
the user's browser (the upload button "did nothing"). This test executes the
whole bundle in a stubbed DOM and drives the upload-settings call graph, so any
missing symbol surfaces as a ReferenceError here instead of on a device.

Requires: node, an up-to-date FilesPageHtml.generated.h (run build_html.py).
"""

import re
import gzip
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GEN = ROOT / "src/network/html/FilesPageHtml.generated.h"
HARNESS = Path(__file__).resolve().parent / "files_page_vm_harness.js"


def main() -> int:
    src = GEN.read_text()
    m = re.search(r"FilesPageHtml\[\] PROGMEM = \{(.*?)\};", src, re.DOTALL)
    if not m:
        print("FAIL: FilesPageHtml payload not found in generated header")
        return 1
    data = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1)))
    html = gzip.decompress(data).decode()

    scripts = re.findall(r"<script>([\s\S]*?)</script>", html)
    out = Path("/tmp/files_scripts_test.js")
    out.write_text("\n;\n".join(scripts))

    proc = subprocess.run(
        ["node", str(HARNESS), str(out)],
        capture_output=True,
        text=True,
        timeout=120,
    )
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)
    return proc.returncode


if __name__ == "__main__":
    sys.exit(main())
