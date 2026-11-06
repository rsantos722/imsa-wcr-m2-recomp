#!/usr/bin/env python3
"""Decode M2 TXTR (M2TX/M2PI/M2CI/M2TA/M2TD/M2PG) textures to PNG.

Format ground truth: 3DO M2 SDK Mercury texture library (txtlib) sources,
publicly available in the trapexit/portfolio_os_m2 dump:
  - M2TX chunk (16 B): u32 reserved, u32 flags (bit0 = IsCompressed),
    u16 MinXSize, u16 MinYSize, u16 TexFormat, u8 NumLOD, u8 Border.
  - LOD dims: lod N is MinX<<(NumLOD-N-1) x MinY<<(NumLOD-N-1); LOD 0 largest.
  - M2TD chunk: u16 NumLOD, u16 pad, u32 LODDataOffset[NumLOD] (from chunk
    data start), then raw per-LOD bytes (last ends at chunk end).
  - M2PI chunk: u32 IndexOffset, u32 reserved, then u32 colors
    [SSB:1][A:7][R:8][G:8][B8]. Alpha expands to 8 bit as (a<<1)|(a&1).
  - Compressed textures (IsCompressed): DCI = M2CI chunk = u16 TexelFormat[4]
    + u32 TxExpColorConst[4]. Bitstream MSB-first; control byte selects
    format = control>>6; transparent formats run (control&0x3F)+1 constant
    pixels; otherwise count=(control&0x1F)+1 with bit5 selecting a string of
    `count` single pixels or a run of one pixel repeated `count` times.
  - CAT bundles contain several FORM TXTR entries -> multiple PNGs.
  - Paged textures (.txp) carry an M2PG chunk describing sub-textures
    (offset/size/format each); each decodes independently from shared M2TD.

Usage:
  python tools/decode_txtr.py "$IMSA_DATA_DIR"/cow.utf -o /tmp/cow.png
  python tools/decode_txtr.py --all --exclude-prefix shooter2d/ \
      --outdir converted/txtr
"""
import argparse
import os
import struct
import sys
import glob

try:
    from PIL import Image
except ImportError:  # Image output is optional for format inspection.
    Image = None



def _matches_prefix(rel_path, prefix):
    """Return whether an extracted-relative path is under ``prefix``."""
    prefix = prefix.replace(os.sep, "/").strip("/")
    return rel_path == prefix or rel_path.startswith(prefix + "/")

# --- DCI / TexFormat flag masks (M2TXTypes.h) ---
M2CI_ISTRANS = 0x0100
M2CI_HASSSB = 0x0200
M2CI_HASCOLOR = 0x0400
M2CI_HASALPHA = 0x0800
M2CI_ISLITERAL = 0x1000
M2PG_HAS_TEX_FORMAT = 0x00000004
M2PG_IS_COMPRESSED = 0x00000080


def parse_chunks(data):
    """Parse a FORM's chunks. Returns (formtype, {cid: payload})."""
    if len(data) < 12 or data[:4] != b'FORM':
        raise ValueError(f"not FORM: {data[:4]!r}")
    sz = struct.unpack('>I', data[4:8])[0]
    if sz < 4:
        raise ValueError(f"short FORM body: {sz}")
    ftype = data[8:12]
    off = 12
    end = 8 + sz
    if end != len(data):
        raise ValueError(
            f"FORM size mismatch: declared end {end}, file {len(data)}"
        )
    chunks = {}
    order = []
    while off < end:
        if off + 8 > end:
            raise ValueError(f"truncated chunk header at 0x{off:x}")
        cid = data[off:off+4]
        csz = struct.unpack('>I', data[off+4:off+8])[0]
        payload_end = off + 8 + csz
        next_off = payload_end + ((-csz) % 4)
        if payload_end > end or next_off > end:
            raise ValueError(f"chunk {cid!r} extends past FORM")
        if cid not in chunks:
            chunks[cid] = data[off+8:payload_end]
            order.append(cid)
        off = next_off
    return ftype, chunks


def iter_forms(data):
    """Yield (offset, form_bytes) for every FORM TXTR/FONT in a file.

    Handles bare FORM files and CAT bundles of FORMs."""
    if data[:4] == b'FORM':
        parse_chunks(data)
        yield 0, data
        return
    if data[:4] == b'CAT ':
        if len(data) < 12:
            raise ValueError("short CAT header")
        cat_size = struct.unpack('>I', data[4:8])[0]
        if cat_size < 4 or 8 + cat_size != len(data):
            raise ValueError("CAT size does not match file")
        # CAT payload lists FORMs back to back after the 12-byte header
        off = 12
        end = len(data)
        while off < end:
            if off + 12 > end or data[off:off+4] != b'FORM':
                raise ValueError(f"CAT member is not FORM at 0x{off:x}")
            sz = struct.unpack('>I', data[off+4:off+8])[0]
            form_end = off + 8 + sz
            next_off = form_end + ((-sz) % 4)
            if form_end > end or next_off > end:
                raise ValueError(f"CAT member extends past CAT at 0x{off:x}")
            form_data = data[off:form_end]
            parse_chunks(form_data)
            yield off, form_data
            off = next_off
        return
    raise ValueError(f"not FORM/CAT: magic {data[:4]!r}")


class BitReader:
    """MSB-first bit reader matching qMemGetBits()."""

    def __init__(self, data):
        self.data = data
        self.pos = 0  # bit position

    def get(self, n, strict=False):
        if strict and self.pos + n > len(self.data) * 8:
            raise ValueError("texture bitstream ended before requested bits")
        v = 0
        for _ in range(n):
            byte = self.data[self.pos >> 3] if (self.pos >> 3) < len(self.data) else 0
            v = (v << 1) | ((byte >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return v

    @property
    def exhausted(self):
        return self.pos >= len(self.data) * 8


def one_extend(depth, val):
    """oneExtend() from qmem.h: scale a `depth`-bit value to 8 bits."""
    if depth <= 0:
        return 0
    if depth >= 8:
        return val & 0xFF
    val &= (1 << depth) - 1
    scaled = val << (8 - depth)
    return scaled | ((0xFF >> depth) if (val & 1) else 0)


def decode_color(color):
    """Decode one M2TX/PIP color word into ``(r, g, b, a)``.

    The word is ``[SSB:1][A:7][R:8][G:8][B:8]``.  This is the exact
    ``M2TXColor_Decode`` operation: the seven-bit alpha is shifted left and
    its low bit is copied into the result's low bit.  The SSB bit is kept
    separately by :func:`decode_pip_entry`; treating the whole top byte as
    alpha would incorrectly turn the SSB bit into opacity and would not be a
    faithful preview of the source format.
    """
    blue = color & 0xFF
    green = (color >> 8) & 0xFF
    red = (color >> 16) & 0xFF
    alpha7 = (color >> 24) & 0x7F
    alpha = (alpha7 << 1) | (alpha7 & 1)
    return red, green, blue, alpha


def decode_pip_entry(color):
    """Decode one M2PI entry as ``(r, g, b, a, ssb)``."""
    red, green, blue, alpha = decode_color(color)
    return red, green, blue, alpha, (color >> 31) & 1


def pip_from_m2pi(payload, include_ssb=False):
    """M2PI chunk -> palette entries.

    The public/default result remains the historical four-component RGBA
    tuple.  The uncompressed source decoder requests the five-component form
    because a source format can carry alpha and SSB independently of the
    palette entry.
    """
    if payload is None or len(payload) < 8:
        return []
    entries = []
    for i in range(8, len(payload) - (len(payload) - 8) % 4, 4):
        color = struct.unpack('>I', payload[i:i+4])[0]
        entries.append(decode_pip_entry(color) if include_ssb
                       else decode_color(color))
    return entries


def m2ta_color_constants(payload, has_color_const=False):
    """Return the two header PIP constants encoded by ``M2TA``.

    The Mercury loader initializes both header constants to opaque black
    (``0x7f000000``) and clears ``HasColorConst``.  An absent color-constant
    flag selects the later raw-conversion fallback of opaque white
    (``0x7fffffff``).  If either M2TA color attribute is present, the loader
    sets the flag and leaves the other initialized constant black.

    ``M2TX_UncomprToM2TXRaw`` selects these constants by the raw SSB for
    formats that have no independent color/alpha field.  Keep the distinction
    even though the four Configure Race ``0xe78`` border forms are indexed and
    do not use the constants.
    """
    if payload is not None and len(payload) < 4:
        raise ValueError("short M2TA color-constant payload")

    attributes = []
    if payload is not None:
        # M2TA starts with one reserved word, followed by (attribute, value)
        # pairs.  Unknown attributes are intentionally ignored, as in the SDK.
        for offset in range(4, len(payload) - 7, 8):
            attr, value = struct.unpack('>II', payload[offset:offset + 8])
            if attr in (8, 9):
                attributes.append((attr, value))

    has_color_const = bool(has_color_const or attributes)
    fallback = decode_pip_entry(
        0x7F000000 if has_color_const else 0x7FFFFFFF)
    constants = [fallback, fallback]
    for attr, value in attributes:
        if attr == 8:       # TXA_PipConstSSB0
            constants[0] = decode_pip_entry(value)
        else:               # TXA_PipConstSSB1
            constants[1] = decode_pip_entry(value)
    return constants


def dci_from_m2ci(payload):
    """M2CI chunk -> (formats[4], consts[4])."""
    if payload is None or len(payload) < 24:
        raise ValueError("missing/short M2CI")
    formats = [struct.unpack('>H', payload[i*2:i*2+2])[0] for i in range(4)]
    consts = [struct.unpack('>I', payload[8+i*4:12+i*4])[0] for i in range(4)]
    return formats, consts


def texel_depth(tex_format):
    """Return the source-defined packed bit depth for one texel."""
    c_depth = (tex_format & 0xF) if tex_format & M2CI_HASCOLOR else 0
    a_depth = ((tex_format >> 4) & 0xF) if tex_format & M2CI_HASALPHA else 0
    ssb_depth = 1 if tex_format & M2CI_HASSSB else 0
    if tex_format & M2CI_ISLITERAL:
        c_depth *= 3
    depth = c_depth + a_depth + ssb_depth
    if depth <= 0:
        raise ValueError("texture page has no texel bit depth")
    return depth


def padded_lod_bytes(pixels, depth):
    """Match the SDK ``padPixels`` 32-bit LOD storage rule."""
    bits = pixels * depth
    return ((bits + 31) // 32) * 4


def decompress_lod(bitstream, formats, consts, n_pixels, verbose=False, strict=False):
    """Decompress one LOD bitstream to palette indices (or None for literal).

    Implements M2TX_ComprToM2TXIndex() from the SDK txtlib."""
    br = BitReader(bitstream)
    out = []
    fmt_cache = {}
    for i, f in enumerate(formats):
        fmt_cache[i] = dict(
            is_trans=bool(f & M2CI_ISTRANS),
            has_ssb=bool(f & M2CI_HASSSB),
            has_color=bool(f & M2CI_HASCOLOR),
            has_alpha=bool(f & M2CI_HASALPHA),
            is_literal=bool(f & M2CI_ISLITERAL),
            c_depth=f & 0xF,
            a_depth=(f >> 4) & 0xF,
            const=consts[i],
        )
    while len(out) < n_pixels:
        if strict and br.pos + 8 > len(bitstream) * 8:
            raise ValueError("texture bitstream ended before a control byte")
        if not strict and br.pos + 8 > len(bitstream) * 8 + 8:
            break
        control = br.get(8, strict=strict)
        fi = (control >> 6) & 3
        f = fmt_cache[fi]
        if f['is_trans']:
            count = (control & 0x3F) + 1
            # Index variant: transIndex = low byte of the constant color
            idx = consts[fi] & 0xFF
            out.extend([idx] * count)
            continue
        count = (control & 0x1F) + 1
        is_string = (control >> 5) & 1
        loop = count if is_string else 1
        for _ in range(loop):
            if f['has_ssb']:
                br.get(1, strict=strict)
            if f['has_alpha'] and f['a_depth'] > 0:
                br.get(f['a_depth'], strict=strict)
            if not f['has_color'] or f['c_depth'] == 0:
                idx = 0
            elif f['is_literal']:
                # Literal RGB: decode straight through to RGBA via sentinel
                r = one_extend(f['c_depth'], br.get(f['c_depth'], strict=strict))
                g = one_extend(f['c_depth'], br.get(f['c_depth'], strict=strict))
                b = one_extend(f['c_depth'], br.get(f['c_depth'], strict=strict))
                idx = ('rgb', r, g, b)
            else:
                idx = br.get(f['c_depth'], strict=strict)
            runlen = 1 if is_string else count
            out.extend([idx] * runlen)
    if verbose:
        print(f"    decompress: consumed {br.pos}/{len(bitstream)*8} bits -> {len(out)} px (want {n_pixels})")
    return out[:n_pixels]


def unpack_indices(data, bpp, count):
    """Unpack `count` big-endian packed indices at `bpp` bits."""
    if bpp == 8:
        return list(data[:count])
    br = BitReader(data)
    return [br.get(bpp) for _ in range(count)]


def decode_uncompressed_texels(data, tex_format, count, palette=None,
                               constants=None, strict=True, index_offset=0):
    """Decode one uncompressed LOD using ``M2TX_UncomprToM2TX*`` semantics.

    The source stream is one continuous MSB-first bitstream.  Each texel
    consumes, in order, its optional SSB bit, optional alpha field, and
    optional literal RGB or indexed color field.  It does not restart at a
    scan-line boundary.  A source alpha/SSB field takes precedence over the
    corresponding PIP entry; when the field is absent, the vendor decoder
    falls back to the PIP entry (indexed color) or the SSB-selected header
    constant (literal/no-color).

    The return value is a list of ``(r, g, b, a, ssb)`` tuples, or ``None``
    for an out-of-range palette index.  The latter preserves the decoder's
    historical magenta diagnostic for malformed source data while keeping
    valid source bytes independent of palette alpha/SSB conventions.
    """
    if count < 0:
        raise ValueError("negative uncompressed texel count")
    has_color = bool(tex_format & M2CI_HASCOLOR)
    has_alpha = bool(tex_format & M2CI_HASALPHA)
    has_ssb = bool(tex_format & M2CI_HASSSB)
    literal = bool(tex_format & M2CI_ISLITERAL)
    color_depth = (tex_format & 0xF) if has_color else 0
    alpha_depth = ((tex_format >> 4) & 0xF) if has_alpha else 0
    source_depth = ((3 * color_depth) if literal else color_depth) \
        + alpha_depth + (1 if has_ssb else 0)
    if strict and len(data) * 8 < count * source_depth:
        raise ValueError(
            f"uncompressed LOD ended before {count} texels "
            f"({len(data) * 8} bits < {count * source_depth})"
        )

    palette = palette or []
    if not constants:
        constants = m2ta_color_constants(None)
    # Accept either the five-component internal form or the historical
    # four-component RGBA form for callers outside this module.
    normalized_constants = []
    for entry in constants:
        if len(entry) >= 5:
            normalized_constants.append(tuple(entry[:5]))
        else:
            normalized_constants.append(tuple(entry[:4]) + (0,))

    br = BitReader(data)
    out = []
    for _ in range(count):
        source_ssb = br.get(1, strict=strict) if has_ssb \
            else normalized_constants[0][4]
        source_alpha = one_extend(alpha_depth, br.get(alpha_depth, strict=strict)) \
            if has_alpha and alpha_depth else None
        palette_index = None
        if not has_color or color_depth == 0:
            constant = normalized_constants[1 if source_ssb else 0]
            rgba = constant[:4]
        elif literal:
            rgba = tuple(
                one_extend(color_depth, br.get(color_depth, strict=strict))
                for _ in range(3)
            ) + (normalized_constants[1 if source_ssb else 0][3],)
        else:
            palette_index = (br.get(color_depth, strict=strict) + index_offset) & 0xFF
            if palette_index >= len(palette):
                rgba = None
            else:
                entry = tuple(palette[palette_index])
                rgba = entry[:4]

        if has_alpha:
            alpha = (source_alpha if alpha_depth else
                     normalized_constants[1 if source_ssb else 0][3])
        elif not has_color or color_depth == 0 or literal:
            alpha = normalized_constants[1 if source_ssb else 0][3]
        elif rgba is not None:
            # Indexed color without a source alpha uses the PIP entry alpha.
            alpha = rgba[3]
        else:
            alpha = normalized_constants[1 if source_ssb else 0][3]

        if has_ssb:
            ssb = source_ssb
        elif not has_color or color_depth == 0 or literal:
            ssb = normalized_constants[0][4]
        elif rgba is not None and palette_index is not None:
            entry = tuple(palette[palette_index])
            ssb = entry[4] if len(entry) >= 5 else 0
        else:
            ssb = normalized_constants[0][4]

        if rgba is None:
            out.append(None)
        else:
            out.append((rgba[0], rgba[1], rgba[2], alpha, ssb))
    return out


def decoded_texels_to_image(texels, w, h):
    """Materialize decoded source texels as an RGBA preview image."""
    if len(texels) != w * h:
        raise ValueError(f"decoded texel count {len(texels)} != {w * h}")
    img = Image.new('RGBA', (w, h))
    pix = img.load()
    for y in range(h):
        for x in range(w):
            texel = texels[y * w + x]
            if texel is None:
                pix[x, y] = (255, 0, 255, 255)
            else:
                pix[x, y] = tuple(texel[:4])
    return img


def indices_to_image(indices, w, h, palette, literal_rgba=None):
    img = Image.new('RGBA', (w, h))
    pix = img.load()
    for y in range(h):
        for x in range(w):
            v = indices[y*w + x]
            if isinstance(v, tuple):  # literal rgb
                pix[x, y] = (v[1], v[2], v[3], 255)
            elif palette and isinstance(v, int) and v < len(palette):
                r, g, b, a = palette[v][:4]
                pix[x, y] = (r, g, b, a)
            else:
                pix[x, y] = (255, 0, 255, 255)
    return img


def validate_indexed_values(values, palette, label):
    """Reject compressed/indexed output that has no usable PIP entry.

    ``decompress_lod`` and ``decode_uncompressed_texels`` retain a magenta
    sentinel for callers that want to inspect malformed data, but a successful
    M2TX decode cannot turn an out-of-range index into a valid-looking PNG.
    The Mercury indexed path requires a PIP for the emitted index; validate it
    before image materialization so the loader gate fails closed.
    """
    indexed = [value for value in values if isinstance(value, int)]
    if not indexed:
        return
    if not palette:
        raise ValueError(f"{label}: indexed texels have no M2PI palette")
    for position, value in enumerate(indexed):
        if value < 0 or value >= len(palette):
            raise ValueError(
                f"{label}: palette index {value} at output texel "
                f"{position} is outside M2PI range {len(palette)}")


def validate_decoded_texels(decoded, palette, label):
    """Validate uncompressed source texels before rendering a preview."""
    missing = [position for position, value in enumerate(decoded)
               if value is None]
    if missing:
        raise ValueError(
            f"{label}: unresolved indexed texel at {missing[0]} "
            "(missing or out-of-range M2PI entry)")
    validate_indexed_values(decoded, palette, label)


class Texture:
    """One decoded TXTR form."""

    def __init__(self, form_data, name=''):
        self.name = name
        ftype, ch = parse_chunks(form_data)
        self.ftype = ftype
        self.chunks = ch
        m2tx = ch.get(b'M2TX')
        if not m2tx or len(m2tx) < 16:
            raise ValueError("missing/short M2TX")
        (self.reserved, self.flags, self.min_x, self.min_y,
         self.tex_format, self.num_lod, self.border) = struct.unpack('>IIHHHBB', m2tx)
        self.is_compressed = bool(self.flags & 1)
        td = ch.get(b'M2TD')
        if td is None or len(td) < 4:
            raise ValueError("missing M2TD")
        n = struct.unpack('>H', td[:2])[0]
        if n != self.num_lod or n == 0:
            raise ValueError(f"M2TD/M2TX LOD mismatch: {n} != {self.num_lod}")
        header_len = 4 + 4 * n
        if header_len > len(td):
            raise ValueError("M2TD offset table extends past chunk")
        offs = [struct.unpack('>I', td[4+4*i:8+4*i])[0] for i in range(n)]
        if any(o < header_len or o >= len(td) for o in offs):
            raise ValueError("M2TD LOD offset is outside texel data")
        if any(left >= right for left, right in zip(offs, offs[1:])):
            raise ValueError("M2TD LOD offsets are not increasing")
        self.lods = []
        for i, o in enumerate(offs):
            nxt = offs[i+1] if i+1 < n else len(td)
            self.lods.append((o, nxt - o))
        self.pip = pip_from_m2pi(ch.get(b'M2PI'))
        self.pip_entries = pip_from_m2pi(ch.get(b'M2PI'), include_ssb=True)
        self.color_constants = m2ta_color_constants(
            ch.get(b'M2TA'), bool(self.flags & 0x4))
        self.dci = dci_from_m2ci(ch[b'M2CI']) if b'M2CI' in ch else (None, None)
        # Paged texture: sub-textures from M2PG
        self.pages = []
        pg = ch.get(b'M2PG')
        if pg is not None:
            if len(pg) < 8:
                raise ValueError("short M2PG")
            num_tex, version = struct.unpack('>II', pg[:8])
            if num_tex == 0:
                raise ValueError("M2PG has no sub-textures")
            off = 8
            # M2TXPgHeader: u32 offset,u32 flags,u16 x,u16 y,u16 fmt,
            # u8 NumLOD + three index bytes = 20 B. Mercury >=3.0 appends
            # TexelFormat[4]+TxExpColorConst[4]+LODLength[NumLOD].
            for t in range(num_tex):
                if off + 20 > len(pg):
                    raise ValueError("M2PG page table is truncated")
                pgo, fl, mx, my, tf, nl = struct.unpack('>IIHHHB', pg[off:off+15])
                if nl == 0 or mx == 0 or my == 0:
                    raise ValueError("M2PG page has invalid dimensions/LOD count")
                effective_tf = (tf if fl & M2PG_HAS_TEX_FORMAT
                                else self.tex_format)
                extension_len = 0
                page_formats = None
                page_consts = None
                lod_lengths = None
                if version >= 0x00030000:
                    if nl > 11:
                        raise ValueError("M2PG page has more than 11 LODs")
                    extension_len = 8 + 16 + 4 * nl
                    if off + 20 + extension_len > len(pg):
                        raise ValueError("M2PG v3 page header extends past chunk")
                    ext = off + 20
                    page_formats = struct.unpack('>4H', pg[ext:ext+8])
                    page_consts = struct.unpack('>4I', pg[ext+8:ext+24])
                    lod_lengths = struct.unpack(
                        '>' + ('I' * nl), pg[ext+24:ext+24 + 4 * nl]
                    )
                self.pages.append(dict(offset=pgo, flags=fl, min_x=mx, min_y=my,
                                       pip_index_offset=pg[off + 15],
                                       tex_format=tf,
                                       effective_tex_format=effective_tf,
                                       num_lod=nl,
                                       page_formats=page_formats,
                                       page_consts=page_consts,
                                       lod_lengths=lod_lengths))
                off += 20 + extension_len
            if off != len(pg):
                raise ValueError("M2PG has unaccounted trailing bytes")

        self.validate_metadata()

    def validate_metadata(self):
        """Validate all bounds that do not require constructing an image."""
        if self.min_x == 0 or self.min_y == 0 or self.num_lod == 0:
            raise ValueError("M2TX has invalid dimensions/LOD count")
        if self.pages:
            td = self.chunks[b'M2TD']
            base = self.lods[0][0]
            data_end = len(td) - base
            page_offsets = [page['offset'] for page in self.pages]
            if any(o < 0 or o > data_end for o in page_offsets):
                raise ValueError("M2PG page offset is outside M2TD data")
            if any(left >= right for left, right in zip(page_offsets, page_offsets[1:])):
                raise ValueError("M2PG page offsets are not increasing")
            for index, page in enumerate(self.pages):
                end = (page_offsets[index + 1]
                       if index + 1 < len(page_offsets) else data_end)
                available = end - page['offset']
                expected = sum(self.page_lod_sizes(page))
                if expected > available:
                    raise ValueError(
                        f"M2PG page {index} short data {available} < {expected}"
                    )
        else:
            for lod in range(self.num_lod):
                _, size = self.lods[lod]
                width, height = self.lod_dims(lod)
                pixels = width * height
                if self.is_compressed:
                    if self.dci[0] is None:
                        raise ValueError("compressed texture has no M2CI")
                else:
                    depth = texel_depth(self.tex_format)
                    expected = (pixels * depth + 7) // 8
                    if size < expected:
                        raise ValueError(
                            f"LOD{lod} short data {size} < {expected}"
                        )

    def validate(self):
        """Validate metadata and decode every LOD/page without writing files."""
        self.validate_metadata()
        if self.pages:
            # `validate_metadata` established the exact bounds; unpacking the
            # index stream also checks that every requested pixel is present.
            td = self.chunks[b'M2TD']
            base = self.lods[0][0]
            data_end = len(td) - base
            for index, page in enumerate(self.pages):
                end = (self.pages[index + 1]['offset']
                       if index + 1 < len(self.pages) else data_end)
                start = page['offset'] + base
                data = td[start:start + end - page['offset']]
                lod_sizes = self.page_lod_sizes(page)
                if sum(lod_sizes) > len(data):
                    raise ValueError(f"M2PG page {index} data is short")
                lod_offset = 0
                for lod, lod_size in enumerate(lod_sizes):
                    lod_data = data[lod_offset:lod_offset + lod_size]
                    width = page['min_x'] << (page['num_lod'] - lod - 1)
                    height = page['min_y'] << (page['num_lod'] - lod - 1)
                    pixels = width * height
                    if page['flags'] & M2PG_IS_COMPRESSED:
                        indices = decompress_lod(
                            lod_data,
                            page['page_formats'],
                            page['page_consts'],
                            pixels,
                            strict=True,
                        )
                        indices = [(i + page['pip_index_offset']) & 0xFF
                                   if isinstance(i, int) else i for i in indices]
                        if len(indices) != pixels:
                            raise ValueError(
                                f"M2PG page {index} LOD{lod} index stream is short"
                            )
                        validate_indexed_values(
                            indices, self.pip_entries,
                            f"M2PG page {index} LOD{lod}")
                    else:
                        decoded = decode_uncompressed_texels(
                            lod_data,
                            page['effective_tex_format'],
                            pixels,
                            self.pip_entries,
                            self.color_constants,
                            strict=True,
                            index_offset=page['pip_index_offset'],
                        )
                        if len(decoded) != pixels:
                            raise ValueError(
                                f"M2PG page {index} LOD{lod} source stream is short"
                            )
                        validate_decoded_texels(
                            decoded, self.pip_entries,
                            f"M2PG page {index} LOD{lod}")
                    lod_offset += lod_size
            return
        for lod in range(self.num_lod):
            width, height = self.lod_dims(lod)
            expected = width * height
            data = self.chunks[b'M2TD'][self.lods[lod][0]:
                                         self.lods[lod][0] + self.lods[lod][1]]
            if self.is_compressed:
                indices = decompress_lod(
                    data, self.dci[0], self.dci[1], expected, strict=True
                )
                if len(indices) != expected:
                    raise ValueError(
                        f"LOD{lod} decoded {len(indices)} < {expected}"
                    )
                validate_indexed_values(
                    indices, self.pip_entries, f"LOD{lod}")
            else:
                decoded = decode_uncompressed_texels(
                    data,
                    self.tex_format,
                    expected,
                    self.pip_entries,
                    self.color_constants,
                    strict=True,
                )
                if len(decoded) != expected:
                    raise ValueError(
                        f"LOD{lod} decoded {len(decoded)} < {expected}"
                    )
                validate_decoded_texels(
                    decoded, self.pip_entries, f"LOD{lod}")

    def page_lod_sizes(self, page):
        """Return source-defined byte lengths for each page LOD."""
        if page['flags'] & M2PG_IS_COMPRESSED:
            if page['lod_lengths'] is None:
                raise ValueError("compressed M2PG page has no LOD lengths")
            if not page['page_formats'] or not page['page_consts']:
                raise ValueError("compressed M2PG page has no DCI")
            if any(length == 0 for length in page['lod_lengths']):
                raise ValueError("compressed M2PG page has an empty LOD")
            return list(page['lod_lengths'])
        depth = texel_depth(page['effective_tex_format'])
        return [
            padded_lod_bytes(
                (page['min_x'] << (page['num_lod'] - lod - 1))
                * (page['min_y'] << (page['num_lod'] - lod - 1)),
                depth,
            )
            for lod in range(page['num_lod'])
        ]

    def lod_dims(self, lod):
        sh = self.num_lod - lod - 1
        return self.min_x << sh, self.min_y << sh

    def decode_lod(self, lod):
        off, size = self.lods[lod]
        data = self.chunks[b'M2TD'][off:off+size]
        w, h = self.lod_dims(lod)
        n_px = w * h
        tf = self.tex_format
        if self.is_compressed:
            formats, consts = self.dci
            if not formats:
                raise ValueError("compressed but no M2CI")
            indices = decompress_lod(data, formats, consts, n_px, strict=True)
            return indices_to_image(indices, w, h, self.pip)
        # Uncompressed source data is a continuous MSB-first stream.  In
        # particular, do not unpack the packed color bytes in isolation: the
        # 0xe78 border form is SSB -> A7 -> C8 per texel.
        decoded = decode_uncompressed_texels(
            data, tf, n_px, self.pip_entries, self.color_constants,
            strict=True,
        )
        # Texel data is stored linear row-major on disc (verified: cow.utf
        # mipmapped 6bpp, ra_cone.txp pages); there is no per-row bit reset.
        return decoded_texels_to_image(decoded, w, h)

    def images(self, errors=None):
        """Return [(suffix, Image)] covering every LOD / page.

        If `errors` is provided, append a description for each required
        LOD/page that could not be decoded.  Callers such as the CLI use that
        sink to distinguish a partial decode from a complete one while still
        keeping every image that was decoded successfully.
        """
        out = []
        if self.pages:
            td = self.chunks[b'M2TD']
            for pi, pgp in enumerate(self.pages):
                try:
                    # page offsets are relative to the texel data area (after
                    # the u16 NumLOD + u16 pad + u32 offsets[numLOD] header);
                    # each page stores all its LODs back to back, LOD0 largest.
                    start = pgp['offset'] + (self.lods[0][0] if self.lods else 8)
                    sh = pgp['num_lod'] - 1
                    w = pgp['min_x'] << sh
                    h = pgp['min_y'] << sh
                    end_off = self.pages[pi+1]['offset'] if pi+1 < len(self.pages) \
                        else len(td) - (self.lods[0][0] if self.lods else 8)
                    data = td[start:start + (end_off - pgp['offset'])]
                    lod_sizes = self.page_lod_sizes(pgp)
                    lod_data = data[:lod_sizes[0]]
                    n_px = w * h
                    if pgp['flags'] & M2PG_IS_COMPRESSED:
                        indices = decompress_lod(
                            lod_data,
                            pgp['page_formats'],
                            pgp['page_consts'],
                            n_px,
                            strict=True,
                        )
                        indices = [(i + pgp['pip_index_offset']) & 0xFF
                                   if isinstance(i, int) else i for i in indices]
                        image = indices_to_image(indices, w, h, self.pip)
                    else:
                        decoded = decode_uncompressed_texels(
                            lod_data,
                            pgp['effective_tex_format'],
                            n_px,
                            self.pip_entries,
                            self.color_constants,
                            strict=True,
                            index_offset=pgp['pip_index_offset'],
                        )
                        image = decoded_texels_to_image(decoded, w, h)
                    out.append((f"page{pi}_{w}x{h}", image))
                except Exception as e:
                    if errors is None:
                        raise
                    message = f"page{pi}: {e}"
                    print(f"  warn {self.name} {message}")
                    errors.append(message)
            return out
        for lod in range(self.num_lod):
            try:
                suffix = f"_mip{lod}" if self.num_lod > 1 else ""
                out.append((suffix, self.decode_lod(lod)))
            except Exception as e:
                message = f"LOD{lod}: {e}"
                print(f"  warn {self.name} {message}")
                if errors is not None:
                    errors.append(message)
                break
        return out


def decode_file(path, out_path=None, verbose=False, input_root=None, errors=None):
    """Decode all TXTR forms in `path`. Returns list of written PNG paths.

    If `errors` is provided, append one description for every FORM, LOD, page,
    or image that failed.  Decoding remains best effort: successfully decoded
    forms are still written and later forms are still attempted.
    """
    data = open(path, 'rb').read()
    root = os.path.abspath(input_root) if input_root else None
    rel = (os.path.relpath(path, root)
           if root and os.path.abspath(path).startswith(root + os.sep)
           else os.path.basename(path))
    failed = errors if errors is not None else []
    written = []
    forms = []
    try:
        # Consume incrementally so a malformed CAT member can still be
        # reported after any preceding, valid members have been collected.
        forms.extend(iter_forms(data))
    except Exception as e:
        message = f"{rel}: {e}"
        print(f"FAIL {message}")
        failed.append(message)
    for fi, (_, fdata) in enumerate(forms):
        try:
            tex = Texture(fdata, name=f"{rel}[{fi}]")
            tex.validate()
        except Exception as e:
            message = f"{rel}[{fi}]: {e}"
            print(f"FAIL {message}")
            failed.append(message)
            continue
        image_errors = []
        try:
            images = tex.images(errors=image_errors)
        except Exception as e:
            message = f"{rel}[{fi}]: {e}"
            print(f"FAIL {message}")
            failed.append(message)
            continue
        for image_error in image_errors:
            message = f"{rel}[{fi}]: {image_error}"
            print(f"FAIL {message}")
            failed.append(message)
        for suffix, img in images:
            if out_path:
                base, ext = os.path.splitext(out_path)
                p = f"{base}{('_%03d' % fi) if len(forms) > 1 else ''}{suffix}{ext}"
                os.makedirs(os.path.dirname(p) or '.', exist_ok=True)
                try:
                    img.save(p)
                except Exception as e:
                    message = f"{rel}[{fi}]{suffix}: {e}"
                    print(f"FAIL {message}")
                    failed.append(message)
                    continue
                written.append(p)
                if verbose:
                    print(f"wrote {p} {img.size}")
        if verbose:
            print(f"{rel}[{fi}]: type {tex.ftype} {tex.min_x}x{tex.min_y} "
                  f"lod{tex.num_lod} compressed={tex.is_compressed} "
                  f"pip={len(tex.pip)} pages={len(tex.pages)}")
    return written


def iter_embedded_forms(data):
    """Scan a binary blob (e.g. .memimage) for embedded FORM TXTR/FONT forms."""
    import re
    for m in re.finditer(b'FORM', data):
        off = m.start()
        if off + 12 > len(data):
            continue
        sz = struct.unpack('>I', data[off+4:off+8])[0]
        if sz < 4 or off + 8 + sz > len(data):
            continue
        form_data = data[off:off+8+sz]
        try:
            parse_chunks(form_data)
        except ValueError:
            continue
        yield off, form_data


def decode_memimage(path, out_base=None, verbose=False, input_root=None):
    """Decode every TXTR embedded in a .memimage. Returns written paths."""
    data = open(path, 'rb').read()
    root = os.path.abspath(input_root) if input_root else None
    rel = (os.path.relpath(path, root)
           if root and os.path.abspath(path).startswith(root + os.sep)
           else os.path.basename(path))
    written = []
    fi = 0
    for _, fdata in iter_embedded_forms(data):
        try:
            tex = Texture(fdata)
            tex.validate()
        except Exception:
            continue
        if out_base:
            base = os.path.join(out_base, rel, f"txtr_{fi:03d}")
            os.makedirs(os.path.dirname(base), exist_ok=True)
        else:
            base = None
        for suffix, img in tex.images():
            if base:
                p = f"{base}{suffix}.png"
                img.save(p)
                written.append(p)
                if verbose:
                    print(f"wrote {p} {img.size}")
        fi += 1
    return written


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input", nargs="*", help="input .utf/.font/.txp/.cel file(s)")
    ap.add_argument("-o", "--output", default=None, help="output PNG (single input)")
    ap.add_argument("--all", action="store_true", help="decode all TXTR files under --root")
    ap.add_argument("--outdir", default="converted/txtr")
    ap.add_argument("--root", default=os.environ.get("IMSA_DATA_DIR"),
                    help="extracted game-data root for --all (default: $IMSA_DATA_DIR)")
    ap.add_argument("--exclude-prefix", action="append", default=[], metavar="PREFIX",
                    help="with --all, skip an extracted-relative path prefix (repeatable)")
    ap.add_argument("--scan-memimage", metavar="FILE", help="list embedded FORMs in a memimage")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if args.scan_memimage:
        data = open(args.scan_memimage, 'rb').read()
        n = 0
        for off, fd in iter_embedded_forms(data):
            print(f"  {hex(off)} FORM {fd[8:12]} size {hex(struct.unpack('>I', fd[4:8])[0])}")
            n += 1
        print(f"{args.scan_memimage}: {n} embedded FORMs")
        return 0

    if args.all:

        if not args.root:

            ap.error("--all needs --root or IMSA_DATA_DIR")
        patterns = ["**/*.utf", "*.utf", "**/*.font", "**/*.txp"]
        all_files = sorted({f for pat in patterns
                            for f in glob.glob(os.path.join(args.root, pat), recursive=True)})
        files = []
        excluded = []
        for f in all_files:
            rel = os.path.relpath(f, args.root).replace(os.sep, "/")
            if any(_matches_prefix(rel, prefix) for prefix in args.exclude_prefix):
                excluded.append(f)
            else:
                files.append(f)
        print(f"decoding {len(files)} files -> {args.outdir}")
        if excluded:
            print(f"excluding {len(excluded)} files by prefix: "
                  f"{', '.join(args.exclude_prefix)}")
        ok = fail = 0
        for f in files:
            rel = os.path.relpath(f, args.root)
            out = os.path.join(args.outdir, rel) + ".png"
            file_errors = []
            try:
                w = decode_file(f, out, verbose=args.verbose, input_root=args.root,
                                errors=file_errors)
            except Exception as e:
                print(f"FAIL {f}: {e}")
                fail += 1
                continue
            if file_errors:
                print(f"FAIL {f}: {len(file_errors)} decode error(s)")
                fail += 1
            elif w:
                ok += 1
            else:
                print(f"EMPTY {f}")
                fail += 1
        print(f"done ok {ok} fail {fail}")
        return 1 if fail else 0

    inputs = args.input
    if not inputs:
        ap.print_help()
        return 1
    failed = 0
    for i, inp in enumerate(inputs):
        out = args.output if (args.output and len(inputs) == 1) \
            else os.path.splitext(inp)[0] + ".png"
        file_errors = []
        try:
            written = decode_file(inp, out, verbose=True, errors=file_errors)
        except Exception as e:
            print(f"FAIL {inp}: {e}")
            written = []
            file_errors.append(str(e))
        if file_errors:
            print(f"FAIL {inp}: {len(file_errors)} decode error(s)")
        if not written:
            print(f"FAIL {inp}: no decoded output")
        if file_errors or not written:
            failed += 1
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, struct.error, EOFError) as exc:
        sys.exit(f"decode_txtr: {exc}")
