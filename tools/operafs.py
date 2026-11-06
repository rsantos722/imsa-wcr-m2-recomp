#!/usr/bin/env python3
"""Read and extract an Opera filesystem (3DO / Panasonic M2) disc image.

Pure standard library, so it needs no third-party extraction tools.

Layout of a 2048-byte-sector image (all integers are big-endian):

  Volume header, block 0:
    0x00 u8   record type (1)
    0x01 5s   sync bytes "ZZZZZ"
    0x06 u8   version
    0x08 32s  comment
    0x28 32s  label
    0x48 u32  volume id
    0x4c u32  block size
    0x50 u32  block count
    0x54 u32  root directory id
    0x58 u32  root directory block count
    0x5c u32  root directory block size
    0x60 u32  last root directory copy index
    0x64 u32  avatar block numbers, (last copy index + 1) entries

  Directory block:
    0x00 i32  next block (relative to the directory start, -1 at the end)
    0x04 i32  previous block
    0x08 u32  flags
    0x0c u32  first free byte
    0x10 u32  offset of the first entry
    ...       entries

  Directory entry (72 bytes + 4 per avatar):
    0x00 u32  flags (low byte: type, 0x02 file, 0x06 volume label,
                     0x07 directory; 0x40000000 last in block,
                     0x80000000 last in directory)
    0x04 u32  id
    0x08 4s   type code
    0x0c u32  block size
    0x10 u32  byte count
    0x14 u32  block count
    0x18 u32  burst
    0x1c u32  gap
    0x20 32s  file name
    0x40 u32  last avatar index
    0x44 u32  avatar block numbers, (last avatar index + 1) entries

Usage:
  python3 tools/operafs.py list    disc.iso
  python3 tools/operafs.py extract disc.iso OUTPUT_DIR
"""
import argparse
import os
import struct
import sys

SECTOR = 2048
TYPE_FILE = 0x02
TYPE_LABEL = 0x06
TYPE_DIR = 0x07
LAST_IN_BLOCK = 0x40000000
LAST_IN_DIR = 0x80000000
ENTRY_FIXED = 0x44
MAX_DEPTH = 32


class OperaFSError(ValueError):
    pass


class Entry:
    def __init__(self, path, flags, type_code, block_size, byte_count,
                 block_count, avatars):
        self.path = path
        self.flags = flags
        self.type_code = type_code
        self.block_size = block_size
        self.byte_count = byte_count
        self.block_count = block_count
        self.avatars = avatars

    @property
    def kind(self):
        return self.flags & 0xFF

    @property
    def is_dir(self):
        return self.kind == TYPE_DIR

    @property
    def is_file(self):
        return self.kind == TYPE_FILE

    @property
    def offset(self):
        return self.avatars[0] * SECTOR


class Volume:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        self.size = os.fstat(self.f.fileno()).st_size
        head = self._read(0, 0x68 + 4 * 8)
        if head[0] != 1 or head[1:6] != b"ZZZZZ":
            raise OperaFSError("not an Opera filesystem image (bad volume header)")
        (self.block_size, self.block_count, self.root_id, self.root_blocks,
         self.root_block_size, last_copy) = struct.unpack(">IIIIII", head[0x4C:0x64])
        if self.block_size != SECTOR:
            raise OperaFSError("unsupported block size %d" % self.block_size)
        if not 0 <= last_copy < 8:
            raise OperaFSError("bad root directory copy count")
        self.root_avatars = list(struct.unpack(">%dI" % (last_copy + 1),
                                               head[0x64:0x64 + 4 * (last_copy + 1)]))
        self.label = head[0x28:0x48].split(b"\0")[0].decode("latin-1")

    def close(self):
        self.f.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _read(self, offset, length):
        self.f.seek(offset)
        data = self.f.read(length)
        if len(data) != length:
            raise OperaFSError("image truncated at offset %#x" % offset)
        return data

    def _read_dir(self, avatar, blocks, prefix, depth):
        if depth > MAX_DEPTH:
            raise OperaFSError("directory nesting too deep")
        data = self._read(avatar * SECTOR, blocks * SECTOR)
        entries = []
        block = 0
        visited = set()
        while block >= 0:
            if block >= blocks:
                raise OperaFSError("directory block chain leaves the directory")
            if block in visited:
                raise OperaFSError(
                    "cyclic directory block chain (block %d revisited)" % block)
            visited.add(block)
            base = block * SECTOR
            nxt, _prev, _flags, free, first = struct.unpack(">iiIII", data[base:base + 20])
            pos = first
            # `free` is zero on directory blocks that do not publish a
            # first-free offset; fall back to the block size as the old
            # extractor did, otherwise the directory parses as empty.
            limit = min(free, SECTOR) if free else SECTOR
            while pos + ENTRY_FIXED <= limit:
                (flags, _id, type_code, bsize, bcount, nblocks, _burst, _gap) = \
                    struct.unpack(">IIIIIIII", data[base + pos:base + pos + 32])
                name_bytes = data[base + pos + 0x20:base + pos + 0x40].split(b"\0")[0]
                if bsize != SECTOR or not name_bytes or any(
                        c < 0x20 or c > 0x7f for c in name_bytes):
                    break
                # The reference tool escapes 0x7f as the literal text !7F;
                # several font names on the retail disc depend on it.
                name = name_bytes.replace(b"\x7f", b"!7F").decode("latin-1")
                last_avatar = struct.unpack(">I", data[base + pos + 0x40:base + pos + 0x44])[0]
                if last_avatar > 63:
                    raise OperaFSError("implausible avatar count in directory entry")
                if pos + 0x44 + 4 * (last_avatar + 1) > SECTOR:
                    raise OperaFSError("directory entry avatar list runs past its block")
                avatars = list(struct.unpack(
                    ">%dI" % (last_avatar + 1),
                    data[base + pos + 0x44:base + pos + 0x44 + 4 * (last_avatar + 1)]))
                path = prefix + name
                tc = type_code.to_bytes(4, "big").decode("latin-1")
                entries.append(Entry(path, flags, tc, bsize, bcount, nblocks, avatars))
                pos += ENTRY_FIXED + 4 * (last_avatar + 1)
                if flags & (LAST_IN_BLOCK | LAST_IN_DIR):
                    break
            block = nxt
        return entries

    def walk(self):
        """Yield every Entry (files and directories) depth-first."""
        def rec(avatar, blocks, prefix, depth):
            for e in self._read_dir(avatar, blocks, prefix, depth):
                yield e
                if e.is_dir:
                    yield from rec(e.avatars[0], e.block_count, e.path + "/", depth + 1)
        yield from rec(self.root_avatars[0], self.root_blocks, "", 0)

    def read_file(self, entry, chunk=1 << 20):
        remaining = entry.byte_count
        self.f.seek(entry.offset)
        while remaining:
            data = self.f.read(min(chunk, remaining))
            if not data:
                raise OperaFSError("image truncated in %s" % entry.path)
            remaining -= len(data)
            yield data


def host_name(name):
    """Escape bytes that are not portable in host file names as !XX."""
    return "".join("!%02X" % ord(c) if ord(c) < 0x20 or ord(c) >= 0x7F or c in '<>:"\\|?*!'
                   else c for c in name)


def safe_join(root, rel):
    parts = [host_name(p) for p in rel.split("/")]
    if any(p in ("", ".", "..") for p in parts):
        raise OperaFSError("unsafe path in image: %r" % rel)
    dest = os.path.normpath(os.path.join(root, *parts))
    if os.path.commonpath([os.path.abspath(root), os.path.abspath(dest)]) != os.path.abspath(root):
        raise OperaFSError("unsafe path in image: %r" % rel)
    return dest


def extract(iso, out_dir, verbose=False):
    count = 0
    with Volume(iso) as vol:
        os.makedirs(out_dir, exist_ok=True)
        for e in vol.walk():
            dest = safe_join(out_dir, e.path)
            if e.is_dir:
                os.makedirs(dest, exist_ok=True)
            elif e.is_file:
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with open(dest, "wb") as out:
                    for chunk in vol.read_file(e):
                        out.write(chunk)
                count += 1
                if verbose:
                    print(e.path)
    return count


def main(argv=None):
    ap = argparse.ArgumentParser(description="Opera filesystem reader")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list", help="list files in an image")
    p.add_argument("iso")
    p = sub.add_parser("extract", help="extract all files")
    p.add_argument("iso")
    p.add_argument("output")
    p.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "list":
            with Volume(args.iso) as vol:
                for e in vol.walk():
                    print("%s%-4s %10d  %s" % ("d" if e.is_dir else "-", e.type_code.strip("\0"),
                                                 e.byte_count, e.path))
        else:
            n = extract(args.iso, args.output, args.verbose)
            print("extracted %d files to %s" % (n, args.output))
    except OperaFSError as exc:
        print("operafs: %s" % exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
