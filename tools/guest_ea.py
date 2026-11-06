#!/usr/bin/env python3
"""guest_ea.py — resolve lifted-C effective addresses and inspect the guest data image.

Why this exists: the transpiled C in build/generated encodes PPC
lis/addi address construction in two DIFFERENT number systems, and mixing
them up silently yields the wrong global:

  1. The instruction comment `/* 0b170: addi r12, r12, 0x6588 */` shows the
     RELOCATION ADDEND, relative to the owning section base (.data or .bss),
     NOT the raw instruction immediate. The real immediate is negative here.
  2. The generated C line right below it is exact:
     `c->gpr[12] = c->gpr[12] + (uint32_t)-14856;`  (-14856 == -0x3A08)
  3. The matching `lis r12, 0` comment shows the ha() halfword (0x18),
     materialized in C as ((uint32_t)(int32_t)(int16_t)24 << 16) == 0x180000.

So for `lis rX, 0x18 ; addi rX, rX, 0x6588` the effective address is

    0x180000 + (-0x3A08) = 0x17C5F8        # via C displacement
    0x176070 + 0x6588    = 0x17C5F8        # via .data addend (same target)

Both agree; the two NOTATIONS differ. This tool makes both explicit and
refuses to guess which one you pasted.

Image layout (single global image, from generated/game_data.c):
    .text  0x100000 .. 0x17606B   (g_recomp_text)
    .data  0x176070 .. 0x181297   (g_recomp_data, bytes + relocation fixups)
    .bss   0x1812A0 .. 0x18B197   (zero-filled at load; NO static image)

Examples:
  # From the C displacement + lis halfword (most reliable):
  python3 tools/guest_ea.py resolve --lis 24 --disp -14856
  python3 tools/guest_ea.py resolve --lis 24 --disp 7120          # view struct

  # From a comment addend, once you know the section (check nearby
  # `sym='.bss'`/`sym='.data'` relocation comments, or just try both):
  python3 tools/guest_ea.py resolve --section data --addend 0x6588
  python3 tools/guest_ea.py resolve --section bss  --addend 0x930

  # Dump the loaded image at an EA (errors clearly for .bss/.text):
  python3 tools/guest_ea.py dump 0x17C5F8 --words 15
  python3 tools/guest_ea.py dump 0x17C5F8 --words 15 --floats

  # Find a 32-bit value anywhere in the loaded data image:
  python3 tools/guest_ea.py find 0x08000000

The `.data` image is not just the raw bytes from the ELF: game_data.c emits
a g_recomp_data_fixups[] table of ADDR32 relocations, and the runtime patches
those absolute guest VAs into memory after loading. This tool applies the
same fixups, so dumps and pointer searches match runtime memory. Reading the
raw bytes instead would show the pre-relocation addends (for example 0x4dc
at 0x176070 where the loader writes 0x17654c).
"""

import argparse
import json
import os
import re
import struct
import sys
from pathlib import Path

DEFAULT_GENERATED = Path(os.environ.get(
    "IMSA_GENERATED_DIR",
    Path(__file__).resolve().parent.parent / "build" / "generated"))
_DATA_DIR = os.environ.get("IMSA_DATA_DIR")
DEFAULT_IMAGE = os.environ.get("IMSA_GAME_ELF") or (
    str(Path(_DATA_DIR) / "driving" / "game") if _DATA_DIR else "")

TEXT_BASE = 0x100000
# Bases are parsed from game_data.c at load time; these are fallbacks.
DATA_BASE = 0x176070
BSS_BASE = 0x1812A0


def _parse_bases(src: str):
    """Extract the image bases/sizes emitted at the top of game_data.c."""
    vals = {}
    for name in ("text_base", "data_base", "bss_base",
                 "text_size", "data_size", "bss_size",
                 "text_file_offset", "data_file_offset", "image_size"):
        m = re.search(
            rf"unsigned(?: long long)? g_recomp_{name} = (0x[0-9a-fA-F]+|\d+);",
            src)
        if m:
            vals[name] = int(m.group(1), 0)
    return vals


def _parse_fixups(src: str, section: str):
    """Extract a generated ADDR32 fixup table: [(section offset, VA), ...].

    The emitted array has a trailing {0, 0} sentinel, so the declared count is
    authoritative — exactly what the runtime loader uses.
    """
    count_m = re.search(
        rf"unsigned g_recomp_{section}_fixup_count = (\d+);", src)
    if not count_m:
        return []
    count = int(count_m.group(1))
    table_m = re.search(
        rf"const uint32_t g_recomp_{section}_fixups\[\]\[2\] = \{{(.*?)\}};",
        src, re.S)
    if not table_m:
        raise SystemExit(
            f"error: g_recomp_{section}_fixup_count is {count} but "
            f"g_recomp_{section}_fixups[] is missing")
    entries = [
        (int(off, 16), int(val, 16))
        for off, val in re.findall(
            r"\{\s*0x([0-9a-fA-F]+)\s*,\s*0x([0-9a-fA-F]+)\s*\}",
            table_m.group(1))
    ]
    if len(entries) < count:
        raise SystemExit(
            f"error: g_recomp_{section}_fixups[] has {len(entries)} entries, "
            f"expected {count}")
    return entries[:count]


class Image:
    """The guest .data image with the generated ADDR32 relocations applied."""

    def __init__(self, generated=None, image=None):
        data_c = Path(generated or DEFAULT_GENERATED) / "game_data.c"
        if not data_c.is_file():
            raise SystemExit(f"error: generated layout not found: {data_c}")
        src = data_c.read_text()
        bases = _parse_bases(src)
        global TEXT_BASE, DATA_BASE, BSS_BASE
        TEXT_BASE = bases.get("text_base", TEXT_BASE)
        DATA_BASE = bases.get("data_base", DATA_BASE)
        BSS_BASE = bases.get("bss_base", BSS_BASE)
        self.text_size = bases.get("text_size")
        self.data_size = bases.get("data_size")
        self.bss_size = bases.get("bss_size")
        file_offset = bases.get("data_file_offset")
        if self.data_size is None or file_offset is None:
            raise SystemExit(f"error: missing data offset/size in {data_c}")
        if not (image or DEFAULT_IMAGE):
            raise SystemExit("error: no guest image; pass --image or set "
                             "IMSA_GAME_ELF or IMSA_DATA_DIR")
        image_path = Path(image or DEFAULT_IMAGE)
        if not image_path.is_file():
            raise SystemExit(f"error: guest image not found: {image_path}")
        blob = image_path.read_bytes()
        end = file_offset + self.data_size
        if end > len(blob):
            raise SystemExit(f"error: {image_path} is shorter than its .data extent")
        data = bytearray(blob[file_offset:end])

        # Mirror the loader: patch every ADDR32 fixup to its absolute guest
        # VA before anything reads the image. Without this, dumps and pointer
        # searches show pre-relocation addends instead of runtime memory.
        self.fixups = _parse_fixups(src, "data")
        for off, val in self.fixups:
            if off + 4 > len(data):
                raise SystemExit(
                    f"error: .data fixup at +0x{off:X} lies outside the "
                    f"{len(data)}-byte .data extent in {data_c}")
            struct.pack_into(">I", data, off, val)
        self.data = bytes(data)

    def section(self, ea: int) -> str:
        d_end = DATA_BASE + len(self.data)
        b_end = BSS_BASE + (self.bss_size or 0)
        if DATA_BASE <= ea < d_end:
            return "data"
        if BSS_BASE <= ea < b_end:
            return "bss"
        if TEXT_BASE <= ea < DATA_BASE:
            return "text"
        return "unmapped"

    def read(self, ea: int, n: int) -> bytes:
        sec = self.section(ea)
        if sec != "data":
            raise SystemExit(
                f"error: 0x{ea:X} is in {sec} — no loadable image "
                f"({'zero-filled at load' if sec == 'bss' else 'lifted as code'}). "
                f"Only .data (0x{DATA_BASE:X}..0x{DATA_BASE + len(self.data):X}) "
                f"has a dumpable image. For {sec}, read it at runtime via a probe."
            )
        off = ea - DATA_BASE
        return self.data[off:off + n]

    def find32(self, value: int) -> list:
        hits = []
        pat = struct.pack(">I", value)
        start = 0
        while True:
            i = self.data.find(pat, start)
            if i < 0:
                break
            hits.append(DATA_BASE + i)
            start = i + 1
        return hits


def sign16(v: int) -> int:
    """Interpret a 16-bit field as signed (PPC addi d field)."""
    return v - 0x10000 if v & 0x8000 else v


def resolve(lis: int, disp: int) -> int:
    """EA = (lis_halfword << 16) + signed displacement."""
    return ((lis & 0xFFFF) << 16) + sign16(disp & 0xFFFF)


def resolve_addend(section: str, addend: int) -> int:
    base = {"data": DATA_BASE, "bss": BSS_BASE, "text": TEXT_BASE}[section]
    return base + addend


def describe(img: Image, ea: int) -> str:
    sec = img.section(ea)
    extra = ""
    if sec == "data":
        extra = f"  (.data +0x{ea - DATA_BASE:X})"
    elif sec == "bss":
        extra = f"  (.bss +0x{ea - BSS_BASE:X})"
    elif sec == "text":
        extra = f"  (.text +0x{ea - TEXT_BASE:X})"
    return f"0x{ea:X} [{sec}]{extra}"


def _image(args) -> Image:
    return Image(getattr(args, "generated", None), getattr(args, "image", None))


def cmd_resolve(args) -> int:
    if (args.lis is None) != (args.disp is None):
        print("error: --lis and --disp must be given together", file=sys.stderr)
        return 2
    img = _image(args)
    candidates = []
    if args.lis is not None:
        disp = args.disp
        # Allow pasting the comment form (positive hex like 0x6588) directly:
        # treat values in [0x8000, 0xFFFF] as negative 16-bit immediates.
        ea = resolve(args.lis, disp)
        candidates.append((f"lis 0x{args.lis & 0xFFFF:X} << 16; disp {disp} "
                           f"(sign-extended {sign16(disp & 0xFFFF)})",
                           ea))
    if args.section:
        if args.addend is None:
            print("error: --section requires --addend", file=sys.stderr)
            return 2
        candidates.append((f"{args.section} addend +0x{args.addend:X}",
                           resolve_addend(args.section, args.addend)))
    elif args.addend is not None:
        # Try both data and bss; report both with section labels.
        for sec in ("data", "bss"):
            candidates.append((f"if .{sec}-relative: addend +0x{args.addend:X}",
                               resolve_addend(sec, args.addend)))
    if not candidates:
        print("error: nothing to resolve (need --lis/--disp and/or --addend)",
              file=sys.stderr)
        return 2
    for label, ea in candidates:
        print(f"{describe(img, ea)}   # {label}")
    return 0


def cmd_dump(args) -> int:
    img = _image(args)
    ea = int(args.ea, 0)
    if args.floats:
        raw = img.read(ea, 4 * args.words)
        flts = struct.unpack(f">{args.words}f", raw)
        for i, f in enumerate(flts):
            print(f"  [{i:3d}] 0x{ea + 4 * i:X}  {f!r}")
    else:
        for i in range(args.words):
            w = struct.unpack(">I", img.read(ea + 4 * i, 4))[0]
            note = ""
            if args.mark is not None and w == args.mark:
                note = f"   <-- matches 0x{args.mark:08X}"
            print(f"  [{i:3d}] 0x{ea + 4 * i:X}  0x{w:08X}{note}")
    return 0


def cmd_find(args) -> int:
    img = _image(args)
    value = int(args.value, 0)
    hits = img.find32(value)
    if not hits:
        print(f"no 32-bit BE occurrence of 0x{value:08X} in .data image")
        return 1
    for h in hits:
        print(describe(img, h))
    return 0


def cmd_info(args) -> int:
    img = _image(args)
    d_end = DATA_BASE + len(img.data)
    b_end = BSS_BASE + (img.bss_size or 0)
    print(f".text  0x{TEXT_BASE:X} .. 0x{TEXT_BASE + (img.text_size or 0):X}")
    print(f".data  0x{DATA_BASE:X} .. 0x{d_end:X}  (loaded image, "
          f"{len(img.data)} bytes, {len(img.fixups)} fixups applied)")
    print(f".bss   0x{BSS_BASE:X} .. 0x{b_end:X}  (zero-filled; no static image)")
    return 0


# Comment-addend traps that already cost real sessions. Each `wrong` value is
# a relocation comment addend misread as a direct EA; `right` is the EA the
# generated C displacement actually addresses. The checker flags live uses of
# `wrong` so the safe path stays the default.
KNOWN_TRAPS = (
    {
        "wrong": 0x1802E0,
        "right": 0x181580,
        "evidence": "resolve --lis 24 --disp 5504 --section bss --addend 0x2e0",
        "note": "second CarSim input record is BSS 0x181580, not .data 0x1802e0",
    },
    {
        "wrong": 0x180B70,
        "right": 0x181E10,
        "evidence": "live GetCarPosition@0x13f68 writes go to BSS 0x181e10/14/18",
        "note": "static .data (300,-1,0) triplet is unrelated to the live X buffer",
    },
    {
        "wrong": 0x180B74,
        "right": 0x181E14,
        "evidence": "live GetCarPosition@0x13f68 writes go to BSS 0x181e10/14/18",
        "note": "static .data (300,-1,0) triplet is unrelated to the live Y buffer",
    },
    {
        "wrong": 0x180B78,
        "right": 0x181E18,
        "evidence": "live GetCarPosition@0x13f68 writes go to BSS 0x181e10/14/18",
        "note": "static .data (300,-1,0) triplet is unrelated to the live Z buffer",
    },
)

_TRAP_RES = [
    (t, re.compile(rf"\b0[xX]0*{t['wrong']:X}\b", re.IGNORECASE))
    for t in KNOWN_TRAPS
]
_COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def cmd_check(args) -> int:
    """Flag live (non-comment) uses of known comment-addend trap EAs."""
    hits = []
    for path in args.files:
        try:
            src = Path(path).read_text()
        except OSError as ex:
            print(f"error: cannot read {path}: {ex}", file=sys.stderr)
            return 2
        code = _COMMENT_RE.sub(" ", src)
        for lineno, line in enumerate(code.splitlines(), 1):
            for trap, rx in _TRAP_RES:
                for m in rx.finditer(line):
                    hits.append({
                        "file": path,
                        "line": lineno,
                        "wrong": f"0x{trap['wrong']:X}",
                        "right": f"0x{trap['right']:X}",
                        "note": trap["note"],
                        "text": line.strip()[:160],
                    })
    if getattr(args, "json", False):
        print(json.dumps({"violations": hits,
                          "clean": not hits,
                          "traps": len(KNOWN_TRAPS)}, indent=2))
    else:
        for h in hits:
            print(f"{h['file']}:{h['line']}: trap EA {h['wrong']} "
                  f"(use {h['right']} instead) — {h['note']}\n"
                  f"    {h['text']}")
        if not hits:
            print(f"clean: no comment-addend trap EAs in "
                  f"{len(args.files)} file(s) ({len(KNOWN_TRAPS)} traps checked)")
    return 1 if hits else 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        description="Resolve lifted-C PPC effective addresses and inspect the guest data image.")
    p.add_argument("--generated", default=str(DEFAULT_GENERATED),
                   help="directory containing game_data.c")
    p.add_argument("--image", default=str(DEFAULT_IMAGE),
                   help="guest ELF image containing the .data extent")
    sub = p.add_subparsers(dest="cmd", required=True)

    pr = sub.add_parser("resolve", help="Resolve an EA from lis/disp or a section addend")
    pr.add_argument("--lis", type=lambda x: int(x, 0),
                    help="lis halfword as printed in the C, e.g. 24 for ((int16_t)24<<16)")
    pr.add_argument("--disp", type=lambda x: int(x, 0),
                    help="the C displacement literal, e.g. -14856 or 7120 "
                         "(values 0x8000..0xFFFF are treated as negative 16-bit)")
    pr.add_argument("--section", choices=("data", "bss", "text"),
                    help="section the comment addend is relative to")
    pr.add_argument("--addend", type=lambda x: int(x, 0),
                    help="addend from the /* addr: addi rX, rX, 0xNNNN */ comment")
    pr.set_defaults(func=cmd_resolve)

    pd = sub.add_parser("dump", help="Dump words/floats at an EA from the loaded .data image")
    pd.add_argument("ea", help="effective address, e.g. 0x17C5F8")
    pd.add_argument("--words", type=int, default=16)
    pd.add_argument("--floats", action="store_true", help="interpret as big-endian float32")
    pd.add_argument("--mark", type=lambda x: int(x, 0), default=None,
                    help="flag words equal to this value (e.g. 0x08000000)")
    pd.set_defaults(func=cmd_dump)

    pf = sub.add_parser("find", help="Find a 32-bit big-endian value in the loaded .data image")
    pf.add_argument("value", help="e.g. 0x08000000")
    pf.set_defaults(func=cmd_find)

    pi = sub.add_parser("info", help="Print image section layout")
    pi.set_defaults(func=cmd_info)

    pc = sub.add_parser(
        "check",
        help="Flag live uses of known comment-addend trap EAs in C sources")
    pc.add_argument("files", nargs="+",
                    help="C sources to scan")
    pc.add_argument("--json", action="store_true",
                    help="machine-readable JSON report on stdout")
    pc.set_defaults(func=cmd_check)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
