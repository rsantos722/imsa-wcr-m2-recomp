#!/usr/bin/env python3
"""Decompress 3DO/M2 loader-compressed ELF sections (SHF_COMPRESS = 0x8).

Python port of the byte-oriented LZSS decoder used by the Portfolio
compression folio / M2 module loader (`FeedDecompressor`, M2 SDK
src/folios/compression/decompress.c). It follows that routine's state machine
and constant names; it is a translation, not a verbatim copy. Format:
  * 4096-byte window, control byte with bit7 set => next 8 ops are literals
    (bit6..bit0 select literal(1)/match(0) per op, MSB-first),
    bit7 clear => first op is a match.
  * match: 2 bytes big-endian; matchPos = word>>4 (0 = end-of-stream),
    matchLen = (word & 0xF) + BREAK_EVEN(=2), copies matchLen+1 bytes.
Usage: python3 tools/m2_lzss.py <infile> [outfile] [--off N --size N]
"""
import argparse

INDEX_BIT_COUNT = 12
LENGTH_BIT_COUNT = 4
WINDOW_SIZE = 1 << INDEX_BIT_COUNT      # 4096
BREAK_EVEN = 2
END_OF_STREAM = 0
MOD_WINDOW = lambda a: a & (WINDOW_SIZE - 1)
CTRL, LIT, MATCH1, MATCH2 = 0, 1, 2, 3


def decompress(data: bytes) -> bytes:
    out = bytearray()
    window = bytearray(WINDOW_SIZE)
    pos = state = 0
    ctrl = nxtflag = 0
    numBytes = len(data)
    buf = memoryview(data)
    bi = 0

    def getbyte():
        nonlocal bi, numBytes
        if numBytes == 0:
            return None
        numBytes -= 1
        b = buf[bi]
        bi += 1
        return b

    while True:
        byte = getbyte()
        if byte is None:
            break
        # STATE_CONTROLBYTE
        nxtflag = 64
        ctrl = byte
        if (ctrl & 128) == 0:
            state = MATCH1
            byte = getbyte()
            if byte is None:
                break
        else:
            state = LIT
            byte = getbyte()
            if byte is None:
                break
        while True:
            if state == LIT:
                out.append(byte)
                window[pos] = byte
                pos = MOD_WINDOW(pos + 1)
                if nxtflag == 0:
                    state = CTRL
                    break
                if ctrl & nxtflag:
                    nxtflag >>= 1
                    byte = getbyte()
                    if byte is None:
                        break
                    continue
                else:
                    nxtflag >>= 1
                    state = MATCH1
                    byte = getbyte()
                    if byte is None:
                        break
            if state == MATCH1:
                mw = byte << 8
                byte = getbyte()
                if byte is None:
                    break
                mw |= byte
                mpos = mw >> LENGTH_BIT_COUNT
                if mpos == END_OF_STREAM:
                    return bytes(out)
                mlen = (mw & ((1 << LENGTH_BIT_COUNT) - 1)) + BREAK_EVEN
                for i in range(mpos, mpos + mlen + 1):
                    b2 = window[MOD_WINDOW(i)]
                    out.append(b2)
                    window[pos] = b2
                    pos = MOD_WINDOW(pos + 1)
                if nxtflag == 0:
                    state = CTRL
                    break
                if ctrl & nxtflag:
                    state = LIT
                else:
                    state = MATCH1
                nxtflag >>= 1
                byte = getbyte()
                if byte is None:
                    break
    return bytes(out)


def section_is_compressed(elf: bytes, sh_flags: int) -> bool:
    return bool(sh_flags & 0x8)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Decompress an M2/3DO loader LZSS stream.")
    ap.add_argument("input")
    ap.add_argument("output", nargs="?")
    ap.add_argument("--off", type=lambda value: int(value, 0), default=0)
    ap.add_argument("--size", type=lambda value: int(value, 0), default=None)
    args = ap.parse_args(argv)
    src = open(args.input, "rb").read()
    dst = args.output or args.input + ".out"
    size = len(src) - args.off if args.size is None else args.size
    res = decompress(src[args.off:args.off + size])
    open(dst, "wb").write(res)
    print(f"{dst}: {len(res)} bytes from {size}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
