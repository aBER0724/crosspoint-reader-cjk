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
/* MEMFIX-PORT: 4096 still verifies RSA-4096 public keys (ISRG Root X1 included; public
   exptmod needs FP_MAX_BITS >= key size) while halving every fast-math bignum again:
   with WOLFSSL_SMALL_STACK each temp is FP_MAX_BITS/8 * 2 bytes on the heap and TLS
   cert verification allocates the peer RsaKey struct (8 fp_ints) in ONE allocation --
   16.5 KB at 8192 vs 8.3 KB at 4096. The fork's fragmented X4 heap (max block ~15-21 KB
   after WiFi startup) could not host the 16.5 KB peerRsaKey during the GitHub release-CDN
   RSA handshake (wolfSSL_connect -342 PEER_KEY_ERROR, AllocKey failed). We only do
   client-side TLS, so private-key CRT ops (which would need 2x key size) never run. */
#undef FP_MAX_BITS
#define FP_MAX_BITS 4096
"""


def patch_user_settings(path: Path) -> None:
    text = path.read_text()
    if MARKER in text:
        text = text.split(MARKER, 1)[0].rstrip()
    path.write_text(text + OVERRIDES + "\n")
    print(f"Patched wolfSSL settings: {path.relative_to(PROJECT_DIR)}")


for settings in PROJECT_DIR.glob(".pio/libdeps/*/Arduino-wolfSSL/src/user_settings.h"):
    patch_user_settings(settings)
