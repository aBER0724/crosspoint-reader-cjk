from pathlib import Path

Import("env")


PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
MARKER = "/* CrossPoint wolfSSL compatibility overrides */"
OVERRIDES = f"""

{MARKER}
#undef NO_DH
#ifndef HAVE_FFDHE_2048
#define HAVE_FFDHE_2048
#endif
/* MEMFIX-PORT: bignum temp budget. With USE_FAST_MATH each mp_int temp is
   FP_MAX_BITS/8*2 bytes on the heap; a TLS handshake keeps several alive at
   once (peer public-key decode + ECDHE/ECDSA scalars), so FP_MAX_BITS=8192
   meant ~2 KB per temp and ~17 KB of handshake scratch on a device whose
   hop-start largest hole is only ~18 KB -- the X4's heap is chronically
   fragmented (idle MaxAlloc 2.4-10 KB). This downloader uses setInsecure()
   (VERIFY_NONE, no CA path) and an ECDHE-only cipher list, so no op needs
   more than RSA-2048/ECDSA P-256. FP_MAX_BITS must stay >= 4096: fast-math
   sizes operands at half FP_MAX_BITS to leave room for intermediate
   products (see tfm.h: "double the size possible for a number"), so RSA-2048
   needs FP_MAX_BITS=4096 -- this is already the floor; do not lower it. */
#undef FP_MAX_BITS
#define FP_MAX_BITS 4096
/* MEMFIX-DIAG: enable func/line on the wolfSSL allocators so the debug
   heap tracker in SecureClient (CROSSPOINT_TLS_HEAP_TRACE, device_test
   only) can attribute every handshake allocation to its exact source
   function. No printing here (that needs WOLFSSL_DEBUG_MEMORY_PRINT).
   Kept out of release builds via the CROSSPOINT_TLS_HEAP_TRACE guard. */
#ifdef CROSSPOINT_TLS_HEAP_TRACE
    #define WOLFSSL_DEBUG_MEMORY
#endif
/* MEMFIX-PORT: sizing the in-struct input buffer (LARGE_STATIC_BUFFERS +
   RECORD_SIZE) was REJECTED on-device. A 4 KiB inline buffer inflates
   sizeof(WOLFSSL) ~9.2 KiB, and constructing the first ClientHello then
   exhausts the X4's fragmented heap (wcSend for the 120-byte ClientHello
   observed at free=3904 / max-alloc=2036), so the server never gets a
   usable handshake and hop 0 hangs. The CDN's ~4.1 KiB single Certificate
   record still needs to be handled WITHOUT inflating the SSL struct; see
   the CDN hop handling in HttpDownloader/SecureClient for the chosen
   solution (kept out of the struct). */
/* #undef RECORD_SIZE
#define RECORD_SIZE 4096
#define LARGE_STATIC_BUFFERS */
"""


def patch_user_settings(path: Path) -> None:
    # Byte-level editing: never decode. user_settings.h may carry bytes in any
    # encoding (history: UTF-8 em-dashes landed next to GBK content on this
    # zh-CN Windows host and pathlib.read_text() with the locale codec then
    # raised UnicodeDecodeError). The C preprocessor treats the file as bytes,
    # so we keep the pre-MARKER prefix untouched and only append our own block.
    MARKER_B = MARKER.encode("ascii")
    raw = path.read_bytes()
    idx = raw.find(MARKER_B)
    prefix = raw[:idx] if idx >= 0 else raw
    if not prefix.endswith(b"\n"):
        prefix += b"\n"
    path.write_bytes(prefix + OVERRIDES.encode("utf-8") + b"\n")
    print(f"Patched wolfSSL settings: {path.relative_to(PROJECT_DIR)}")


for settings in PROJECT_DIR.glob(".pio/libdeps/*/Arduino-wolfSSL/src/user_settings.h"):
    patch_user_settings(settings)
