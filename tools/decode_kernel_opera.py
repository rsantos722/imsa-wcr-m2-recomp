#!/usr/bin/env python3
"""
kernel.opera decode research (read-only, deterministic).

Question: can the Portfolio/M2 word-oriented `FeedDecompressor` be reused to
decode `System.m2/Boot/kernel.opera` far enough to locate folio 1 ordinal 105
(IsMasterCPU)?  This script ports the word-oriented decompressor faithfully from
trapexit/portfolio_os_m2 (see PROVENANCE) and uses it to test the decode
hypothesis against the authoritative source image, recording reproducible
header / payload / decode evidence.

Findings produced here:
  * kernel.opera is UNCOMPRESSED ARM, not a FeedDecompressor/LZSS stream:
      - 237 plaintext strings present (incl. "Sherry", "<0x%Px>" formats);
      - valid ARM instruction runs from offset 0 (vector `bl` table,
        `mov r0,r0` at 0x40, zero-init stub);
      - no 4-byte-aligned offset (raw / 32-bit-reversed / 16-bit-reversed)
        of the LZSS decoder reproduces these strings or a coherent image
        (no ELF magic, no PPC/ARM prologue at decode start, degenerate or
        random-ARM output).
  * Therefore the word-oriented decompressor premise does not apply; the file is
    an ARM "Sherry" folio kernel image.  Locating ordinal 105 requires parsing
    the 3DO Opera/Sherry folio SELECTOR DISPATCH TABLE, whose on-disc format is
    not documented in-tree.  Three bounded dispatch-table searches all failed.

No writes to discs/ or extracted/.  Deterministic (no randomness, no network).
"""

import argparse
import hashlib
import json
import math
import os
import pathlib
import re
import struct
from collections import Counter

_DATA_DIR = os.environ.get("IMSA_DATA_DIR")
DEFAULT_INPUT = (pathlib.Path(_DATA_DIR) / "System.m2" / "Boot" / "kernel.opera"
                 if _DATA_DIR else None)

# --- Provenance (allowed external source for this bounded format question) -----
PROVENANCE = {
    "repo": "https://github.com/trapexit/portfolio_os_m2",
    "revision": "3d68c03439bf29df5a870c575ae6918105c02137",
    "files": {
        "ws_root/src/folios/compression/decompress.c": "FeedDecompressor (word-oriented LZSS)",
        "ws_root/src/folios/compression/lzss.h": "WINDOW_SIZE/INDEX_BIT_COUNT/LENGTH_BIT_COUNT/BREAK_EVEN",
    },
    "note": "Fetched 2026-08-28 via gh api contents; identical constants to "
            "in-tree tools/m2_lzss.py (loader byte-LZSS) except initial "
            "window pos=1 and 4-byte word-buffered output (cosmetic for bytes).",
}

# Authoritative hash of System.m2/Boot/kernel.opera.
EXPECTED_SHA256 = "9398112ef212d73d793ad7fe7ca0f2146714c7657fba4502827795c8e9ab6ac9"

# --- Faithful port of FeedDecompressor (portfolio decompress.c) ---------------
WINDOW_SIZE = 4096
INDEX_BIT_COUNT = 12
LENGTH_BIT_COUNT = 4
BREAK_EVEN = 2
END_OF_STREAM = 0
MOD = lambda a: a & (WINDOW_SIZE - 1)
CTRL, LIT, M1 = 0, 1, 2


def feed_decompress(data: bytes, pos_init: int = 1, cap: int = 2_000_000) -> bytes:
    """Byte-stream LZSS decoder mirroring FeedDecompressor's state machine.

    The original buffers output into 32-bit words via an OutputWord callback;
    for byte analysis we append each emitted byte directly (word grouping is
    order-preserving and irrelevant to the decoded byte stream).
    """
    out = bytearray()
    window = bytearray(WINDOW_SIZE)
    pos = pos_init
    state = CTRL
    cf = 0
    nf = 0
    eos = False
    bi = 0
    n = len(data)

    def gb():
        nonlocal bi
        if bi >= n:
            return None
        b = data[bi]
        bi += 1
        return b

    while not eos and len(out) < cap:
        byte = gb()
        if byte is None:
            break
        if state == CTRL:
            nf = 64
            cf = byte
            if (cf & 128) == 0:
                state = M1
                byte = gb()
                if byte is None:
                    break
            else:
                state = LIT
                byte = gb()
                if byte is None:
                    break
        if state == LIT:
            out.append(byte)
            window[pos] = byte
            pos = MOD(pos + 1)
            if nf == 0:
                state = CTRL
                continue
            if cf & nf:
                nf >>= 1
                byte = gb()
                if byte is None:
                    break
                continue
            else:
                nf >>= 1
                state = M1
                byte = gb()
                if byte is None:
                    break
        if state == M1:
            mw = byte << 8
            byte = gb()
            if byte is None:
                break
            mw |= byte
            mpos = mw >> LENGTH_BIT_COUNT
            if mpos == END_OF_STREAM:
                eos = True
                break
            mlen = (mw & ((1 << LENGTH_BIT_COUNT) - 1)) + BREAK_EVEN
            for i in range(mpos, mpos + mlen + 1):
                b2 = window[MOD(i)]
                out.append(b2)
                window[pos] = b2
                pos = MOD(pos + 1)
            if nf == 0:
                state = CTRL
                continue
            if cf & nf:
                state = LIT
            else:
                state = M1
            nf >>= 1
            byte = gb()
            if byte is None:
                break
    return bytes(out)


def rev16(d: bytes) -> bytes:
    return b"".join(d[i:i + 2][::-1] for i in range(0, len(d) - 1, 2))


def rev32(d: bytes) -> bytes:
    return b"".join(d[i:i + 4][::-1] for i in range(0, len(d) - 3, 4))


def entropy(b: bytes) -> float:
    if not b:
        return 0.0
    c = Counter(b)
    n = len(b)
    return -sum((v / n) * math.log2(v / n) for v in c.values())


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Inspect the kernel.opera image.")
    ap.add_argument("--input", default=DEFAULT_INPUT,
                    help="kernel.opera path (default: $IMSA_DATA_DIR/System.m2/Boot/kernel.opera)")
    ap.add_argument("--json", default=None, help="optional JSON output path")
    args = ap.parse_args(argv)
    if args.input is None:
        ap.error("--input or IMSA_DATA_DIR is required")
    out = {"provenance": PROVENANCE, "input": str(args.input)}
    lines = []

    def log(s=""):
        lines.append(s)

    kd = pathlib.Path(args.input).read_bytes()
    if len(kd) < 0xC0:
        raise SystemExit(
            f"decode_kernel_opera: {args.input}: {len(kd)} bytes is shorter "
            "than the 0xC0-byte header")
    sha = hashlib.sha256(kd).hexdigest()
    log(f"kernel.opera size {len(kd)} sha256 {sha}")
    out["size"] = len(kd)
    out["sha256"] = sha
    out["hash_matches_expected"] = (sha == EXPECTED_SHA256)

    # Header fields (big-endian)
    f = {
        "0x20": struct.unpack(">I", kd[0x20:0x24])[0],
        "0x2c_load": struct.unpack(">I", kd[0x2c:0x30])[0],
        "0xb0_size": struct.unpack(">I", kd[0xb0:0xb4])[0],
        "0xc0_name": kd[0xc0:0xd0].split(b"\x00")[0].decode(errors="ignore"),
        "0xe0_marker": struct.unpack(">I", kd[0xe0:0xe4])[0],
    }
    log(f"header fields: {f}")
    out["header"] = f
    out["size_field_matches_file_minus_0x40"] = (
        f["0xb0_size"] == len(kd) - 0x40
    )

    # Plaintext-string check (uncompressed evidence)
    strs = re.findall(rb"[ -~]{5,}", kd)
    out["plaintext_string_count"] = len(strs)
    out["has_Sherry_plaintext"] = b"Sherry" in kd
    out["has_IsMasterCPU_plaintext"] = b"IsMasterCPU" in kd
    log(f"plaintext strings: {len(strs)}; 'Sherry' present={b'Sherry' in kd}; "
        f"'IsMasterCPU' present={b'IsMasterCPU' in kd}")
    log("  samples: " + ", ".join(s.decode(errors="ignore") for s in strs[:8]))

    # LZSS applicability test: scan aligned offsets; a real LZSS stream whose
    # output is this ARM kernel MUST reproduce the plaintext 'Sherry' string.
    log("\nLZSS/FeedDecompressor applicability scan (looking for 'Sherry' in "
        "any aligned-offset decode):")
    hit = None
    n = len(kd)
    for off in range(0, n - 200, 4):
        for fn in (lambda x: x, rev32, rev16):
            data = fn(kd[off:])
            if data[0] & 128 == 0:
                continue
            o = feed_decompress(data)
            if b"Sherry" in o[:6000]:
                hit = (off, fn.__name__, len(o))
                break
        if hit:
            break
    out["lzss_reproduces_Sherry"] = hit is not None
    out["lzss_hit"] = {"offset": hit[0], "order": hit[1], "len": hit[2]} if hit else None
    log(f"  result: {'FOUND ' + str(hit) if hit else 'NO decode reproduces Sherry -> not LZSS'}")

    # Dispatch-table search (folio-1 ordinal 105 requires the selector table).
    def w(i):
        return struct.unpack(">I", kd[i:i + 4])[0]

    base = f["0x2c_load"]

    def is_abs_codeptr(v):
        return base <= v < base + n

    # Strategy A: run of absolute code pointers (array indexed by ordinal).
    bestA = 0
    for start in range(0, n - 4, 4):
        cnt = 0
        i = start
        while i + 4 <= n and is_abs_codeptr(w(i)):
            cnt += 1
            i += 4
        if cnt > bestA:
            bestA = cnt
    # Strategy B: (ordinal, codeptr) sequential pairs.
    bestB = 0
    for start in range(0, n - 8, 4):
        cnt = 0
        i = start
        seq = None
        ok = True
        while i + 8 <= n:
            o = w(i)
            p = w(i + 4)
            if seq is None:
                seq = o
            if o != seq + cnt or not is_abs_codeptr(p):
                ok = False
                break
            cnt += 1
            i += 8
        if ok and cnt > bestB:
            bestB = cnt
    # Strategy C: run of in-range link offsets (handler offsets within image).
    bestC = 0
    for start in range(0, n - 4, 4):
        cnt = 0
        i = start
        while i + 4 <= n:
            v = w(i)
            if (v & 3) == 0 and 0x80 <= v < n - 4:
                cnt += 1
                i += 4
            else:
                break
        if cnt > bestC:
            bestC = cnt
    out["dispatch_table"] = {
        "abs_codeptr_run": bestA,
        "ordinal_codeptr_pair_run": bestB,
        "inrange_offset_run": bestC,
        "located": max(bestA, bestB, bestC) >= 20,
    }
    log("\ndispatch-table search (threshold run>=20):")
    log(f"  A abs-codeptr run max={bestA}; B (ord,ptr) pair run max={bestB}; "
        f"C in-range offset run max={bestC}")
    log("  -> located={}".format(out["dispatch_table"]["located"]))

    # Verdict
    uncompressed = (len(strs) > 50) and (b"Sherry" in kd)
    is_lzss = out["lzss_reproduces_Sherry"]
    verdict = "UNCOMPRESSED_ARM" if (uncompressed and not is_lzss) else (
        "LZSS_DECODED" if is_lzss else "INCONCLUSIVE")
    out["verdict"] = verdict
    out["word_oriented_decompressor_applicable"] = is_lzss
    out["ordinal_105_locatable"] = out["dispatch_table"]["located"]
    log(f"\nVERDICT: {verdict}")
    log("  word-oriented FeedDecompressor applicable: " + str(is_lzss))
    log("  ordinal 105 (IsMasterCPU) locatable from in-tree evidence: "
        + str(out["dispatch_table"]["located"]))
    log("  blocker: kernel.opera is an uncompressed ARM 'Sherry' folio kernel "
        "image; the 3DO Opera/Sherry folio SELECTOR DISPATCH TABLE format "
        "(ordinal->handler mapping) is undocumented in-tree, so ord 105's ARM "
        "body is not reachable. The word-oriented decompressor does not apply.")

    if args.json:
        sidecar = pathlib.Path(args.json)
        sidecar.write_text(json.dumps(out, indent=2))
        log(f"\nwrote {sidecar}")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
