#!/usr/bin/env python3
"""Decode 3DO/M2 packed-cel files (CCB / PDAT / PLUT chunks) to PNG.

This cracks `Message.cel` — the last undecoded art format from the
disc triage (docs/formats/memimage.md). Ground truth is the public 3DO SDK
"Creating Cel Source Data" chapter (trapexit/3DO-information,
ppgfldr/ggsfldr/gpgfldr/5gpgd.html):

* each row starts word-aligned; for 8/16 bpp the first word carries a
  10-bit offset in bits 25-16 (bits 31-26 zero) equal to the distance from
  this row's first word to the next row's first word MINUS 2 words;
  for <=6 bpp it is an 8-bit offset in bits 31-24 with no pad bits;
* after the offset, rows are bit-packed MSB-first packets:
    01 literal   (6-bit count+1, N-bit pixels)
    10 transparent (6-bit count+1, no data)
    11 repeat    (6-bit count+1, one N-bit pixel)
    00 end-of-row;
* dimensions come from PRE0.VCNT (bits 15-6, +1) and the CCB tail
  Width/Height fields; bpp code = PRE0 & 7.

Message.cel specifics: PRE0=0x000011C5 (VCNT=71 -> h=72, bpp code 5), W=206,
H=72. The 8-bit values are 3DO *coded* pixels: the low 5 bits index the
trailing 32-entry PLUT (RGB555 grayscale ramp), the high bits are the code
tag — so the decoder maps value & 0x1F through the PLUT. Literal packets
carry count+1 pixels (per the SDK table); a single-value-literal reading
smears bands and desyncs the row bitstream. Transparent packets
appear on two rows and become alpha-0.

Decoded content (verified visually): the Opera wrong-console message —
Japanese + "Sorry, this CD is for M2." + "Panasonic" — referenced by the
`LaunchMe` boot script, i.e. shown when the disc is booted on non-M2 hardware.

Usage:
  python3 tools/decode_message_cel.py "$IMSA_DATA_DIR"/Message.cel -o out.png
"""
import argparse
import struct

PACKETS = {"literal": 1, "transparent": 2, "repeat": 3, "eol": 0}

# PRE0 & 7 -> bits per pixel (3DO hardware.h bppTable; code 5 is the second
# 8-bit encoding, which Message.cel uses as LINEAR 8-bit luminance).
BPP_TABLE = {0: 1, 1: 2, 2: 4, 3: 6, 4: 8, 5: 8, 6: 16}


class Bitstream:
    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0

    def get(self, n: int) -> int:
        v = 0
        for _ in range(n):
            if self.pos >= len(self.data) * 8:
                raise EOFError("bitstream exhausted")
            byte = self.data[self.pos >> 3]
            v = (v << 1) | ((byte >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return v


def parse_chunks(data: bytes):
    out = {}
    off = 0
    while off + 8 <= len(data):
        cid = data[off:off + 4]
        size = struct.unpack(">I", data[off + 4:off + 8])[0]
        # A zero-size chunk (or one that runs past the buffer) would leave
        # `off` unchanged and spin forever on malformed input.
        if size < 8 or off + size > len(data):
            raise ValueError(
                f"invalid chunk {cid!r} at {off:#x}: size {size} "
                f"(buffer {len(data)} bytes)")
        out[cid] = (size, data[off + 8:off + size])
        off += size
    return out


def ccb_fields(body: bytes):
    vals = struct.unpack(">18I", body[:72])
    return {
        "pre0": vals[14],
        "pre1": vals[15],
        "width": vals[16],
        "height": vals[17],
    }


def decode_packed(data: bytes, width: int, height: int, bpp: int, offset_bits: int,
                  pad_bits: int):
    """Returns list of rows; None pixel == transparent packet."""
    bs = Bitstream(data)
    rows = []
    for _ in range(height):
        start_word = bs.pos // 32
        if pad_bits:
            bs.get(pad_bits)
        offset = bs.get(offset_bits)
        row = [None] * width
        x = 0
        while x < width:
            ptype = bs.get(2)
            if ptype == PACKETS["eol"]:
                break
            count = bs.get(6) + 1
            if ptype == PACKETS["transparent"]:
                x += count
                continue
            if ptype == PACKETS["literal"]:
                for _ in range(count):
                    if x < width:
                        row[x] = bs.get(bpp)
                    else:
                        bs.get(bpp)  # keep bitstream aligned past row end
                    x += 1
                continue
            value = bs.get(bpp)
            for _ in range(count):
                if x < width:
                    row[x] = value
                x += 1
        rows.append(row)
        # next row: current start + stored offset (+2 per spec)
        bs.pos = (start_word + offset + 2) * 32
    return rows


def parse_plut(payload: bytes):
    """Trailing PLUT: u32 entry count + u16 RGB555 entries."""
    import struct as _struct

    count = _struct.unpack(">I", payload[:4])[0]
    return [_struct.unpack(">H", payload[4 + i * 2:6 + i * 2])[0]
            for i in range(count)]


def rgb555(v: int) -> tuple:
    return (((v >> 10) & 0x1F) * 255 // 31,
            ((v >> 5) & 0x1F) * 255 // 31,
            (v & 0x1F) * 255 // 31)


def write_png(path: str, rows, width: int, height: int, palette):
    """Minimal RGB/RGBA PNG writer (no external deps)."""
    import zlib

    def chunk(tag: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    raw = bytearray()
    for row in rows:
        raw.append(0)  # filter none
        for px in row:
            if px is None:
                raw += bytes((0, 0, 0, 0))
                continue
            # 3DO coded pixel: low 5 bits index the 32-entry PLUT.
            r, g, b = rgb555(palette[px & 0x1F])
            raw += bytes((r, g, b, 255))

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
           chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input")
    ap.add_argument("-o", "--out", default=None, help="output PNG path")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    data = open(args.input, "rb").read()
    chunks = parse_chunks(data)
    if b"CCB " not in chunks or b"PDAT" not in chunks:
        raise SystemExit("not a cel file (missing CCB/PDAT)")
    fields = ccb_fields(chunks[b"CCB "][1])
    pdat = chunks[b"PDAT"][1]
    palette = parse_plut(chunks[b"PLUT"][1]) if b"PLUT" in chunks else None
    if args.verbose:
        print(f"PLUT entries: {len(palette) if palette else 0}")
    w, h = fields["width"], fields["height"]
    bpp_code = fields["pre0"] & 7
    bpp = BPP_TABLE.get(bpp_code, 8)
    offset_bits, pad_bits = (10, 6) if bpp >= 8 else (8, 0)
    linear = bool(fields["pre0"] & 0x10)
    if args.verbose:
        print(f"CCB: {w}x{h} pre0={fields['pre0']:#010x} "
              f"pre1={fields['pre1']:#010x} bpp_code={bpp_code} bpp={bpp} "
              f"linear={linear}")
    rows = decode_packed(pdat, w, h, bpp, offset_bits, pad_bits)
    consumed_rows = len(rows)
    if args.verbose:
        transparent = sum(1 for r in rows for p in r if p is None)
        print(f"rows decoded: {consumed_rows}/{h}, transparent px: {transparent}")

    out = args.out or (args.input.rsplit(".", 1)[0] + ".png")
    write_png(out, rows, w, h, palette or list(range(32)))
    print(f"wrote {out} ({w}x{h})")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, struct.error, EOFError, OSError) as exc:
        raise SystemExit(f"decode_message_cel: {exc}")
