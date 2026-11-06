# Mercury Textures (`.utf`, `.txp`), `.memimage` Bundles, Fonts, and `Message.cel`

2D UI sprites, 3D vehicle and track textures, bitmap fonts, and pre-serialized 3D scene graphs use the **3DO M2 Mercury (`M2TX`)** texture format and the game's custom **`.memimage`** container.

Standalone decoders are provided in [`tools/decode_txtr.py`](../../tools/decode_txtr.py) and [`tools/decode_message_cel.py`](../../tools/decode_message_cel.py).

## Container Overview

| Extension / Container | Top-Level Magic | Description |
|---|---|---|
| `.utf` (Single) | `FORM .... TXTR` | Single IFF `FORM TXTR` Mercury texture. |
| `.utf` (Bundle) | `CAT  .... TXTR` | Concatenated IFF `CAT` archive of multiple `FORM TXTR` textures (e.g., `System.m2/Requester/StorMgr.utf` with 137 textures, `shooter2d/*.utf`). |
| `.txp` | `FORM .... TXTR` | Paged texture atlas containing an `M2PG` (`PCLT`) sub-texture directory sharing a single `M2TD` texel block and `M2PI` palette (e.g., `driving/data/tracks/*/ra_cone.txp`). |
| `.font` (M2) | `FORM .... FONT` | Contains an embedded `FORM TXTR` glyph atlas plus an `M2DB` glyph metrics chunk. |
| `.font` (M1 legacy) | `FORM .... FONT` | Legacy 3DO `FONT` chunk without `TXTR` (`Courier_10`, `Helvetica_10`, `Helvetica_16`, `StorMgr_9.font`). |
| `.memimage` | Big-endian payload size at `+0x00` | Relocatable payload followed by export and relocation tables; may contain textures, Mercury `Pod` meshes, MPEG stills, or track data. |
| `Message.cel` | `CCB ` (`0x43434220`) | Legacy 3DO Opera packed 8-bit coded Cel (`206×72`) displayed when booted on an original 3DO console. |

## `.memimage` Binary Container Layout

Files include `driving/data/ui/uiart.memimage`, `driving/data/tracks/shared/cars1.memimage` through `cars4.memimage`, `driving/data/showroom/showroom.0.memimage` through `showroom.3.memimage`, and the track packs. All integer fields are big-endian; export and relocation array entries are payload-relative byte offsets:

| Offset | Size | Type | Description |
|---|---|---|---|
| `0x00` | `4` | `u32` | `bsize`: payload size in bytes. |
| `0x04` | `bsize` | `u8[]` | Payload, allocated as one block by the loader. |
| `0x04 + bsize` | `4` | `u32` | `nt`: number of export table entries. |
| `0x08 + bsize` | `4 * nt` | `u32[]` | `nt` exported payload offsets (`0xffffffff` maps to a null export). |
| `0x08 + bsize + 4*nt` | `4` | `u32` | `nr`: number of internal pointer relocations. |
| `0x0c + bsize + 4*nt` | `4 * nr` | `u32[]` | Payload offsets of words to relocate by adding the allocated payload base. |

`readinmemimage` (`0x1f638`) allocates a runtime wrapper and patches the payload's pointer words in-place, while `getnextmemimagepointer` (`0x1f8e0`) steps through the export array (mapping `0xffffffff` to `NULL`). For example, `uiart.memimage` has `bsize = 1020832` and `nt = 105` at file offset `1020836`. The [Car Viewer parser](../../launcher/asset_model.cpp) reads this on-disk layout directly.

Each export entry points to a self-contained record inside the `.memimage` payload:
- In `uiart.memimage`: UI `FORM TXTR` sprites and 16 embedded MPEG-1 intra still frames (`MPS`). The still at file byte `866,404` (`0xd3864`) replaces the standard 12-byte `576×432` MPEG sequence header with a 12-byte prefix followed by a GOP start code at `0xd3870` (packet `+12`) and a picture start code at packet `+20`.
- In race car and showroom packs: 3D Mercury `Pod` scene nodes, geometry, strip/fan indices, materials, and texture-page descriptors plus embedded `FORM TXTR` textures.

## Mercury `FORM TXTR` Chunk Specification

Inside any `FORM TXTR` container, chunks are big-endian (see [IFF Container and Parser](../m2/iff.md)).

### `M2TX` Header Chunk (`16` bytes)

| Offset | Size | Type | Field | Description |
|---|---|---|---|---|
| `+0x00` | `4` | `u32` | `Reserved` | Version / flags (`0`). |
| `+0x04` | `4` | `u32` | `Flags` | Bit `0` (`0x01`): `M2TX_IS_COMPRESSED` (uses `M2CI` DCI RLE compression). |
| `+0x08` | `2` | `u16` | `MinXSize` | Width of the smallest mipmap level (`LOD[NumLOD - 1]`). |
| `+0x0A` | `2` | `u16` | `MinYSize` | Height of the smallest mipmap level (`LOD[NumLOD - 1]`). |
| `+0x0C` | `2` | `u16` | `TexFormat` | Bitfield defining texel bit depths and channels (see below). |
| `+0x0E` | `1` | `u8` | `NumLOD` | Number of mipmap levels (`1..12`). Finest level `LOD 0` has dimensions `MinXSize << (NumLOD - 1)` by `MinYSize << (NumLOD - 1)`. |
| `+0x0F` | `1` | `u8` | `Border` | Border flags. |

#### `TexFormat` (`u16`) Bitfield

| Bits / Mask | Field | Description |
|---|---|---|
| `bits 3..0` (`0x000F`) | `cDepth` | Bits per color component (if `IsLiteral`) or palette index bit width (`0..8`). |
| `bits 7..4` (`0x00F0`) | `aDepth` | Alpha channel bit width (`0`, `4`, or `7`). |
| `bit 8` (`0x0100`) | `IsTrans` | Transparent color-key flag (used in `M2CI` DCI formats: run of constant color `TxExpColorConst[fmt]`). |
| `bit 9` (`0x0200`) | `HasSSB` | 1-bit Source Select Bit present in each texel. |
| `bit 10` (`0x0400`) | `HasColor` | Color/index bits present (`cDepth > 0`). |
| `bit 11` (`0x0800`) | `HasAlpha` | Alpha bits present (`aDepth > 0`). |
| `bit 12` (`0x1000`) | `IsLiteral` | `1` = direct RGB (`3 * cDepth` color bits: R, G, B); `0` = indexed via `M2PI` PIP palette (`cDepth` bits). |

### `M2PI` Pen Index Palette (PIP) Chunk

| Offset | Size | Type | Field | Description |
|---|---|---|---|---|
| `+0x00` | `4` | `u32` | `IndexOffset` | Base PIP index offset. |
| `+0x04` | `4` | `u32` | `Reserved` | Reserved (`0`). |
| `+0x08` | `4 * N` | `u32[N]` | `PIPData` | Palette entries packed as `[SSB:1][A:7][R:8][G:8][B:8]`. 7-bit alpha `A` expands to 8-bit via `(A << 1) | (A & 1)`. |

### `M2CI` Data Compression Information (DCI) Chunk (`24` bytes)

Present when `M2TX.Flags & 1` (`IsCompressed`) is set:
- `+0x00`: `u16 TexelFormat[4]` — four per-packet `TexFormat` descriptors selected by the 2-bit format code in the compressed bitstream.
- `+0x08`: `u32 TxExpColorConst[4]` — four constant color/PIP index values used when the corresponding `TexelFormat[fmt]` has `IsTrans` (`0x0100`) set.

### `M2TD` Texel Data Chunk

- `+0x00`: `u16 NumLOD`, `u16 Reserved`.
- `+0x04`: `u32 LODDataOffset[NumLOD]` — byte offsets (relative to the start of `LODDataOffset[0]`, i.e., `M2TD + 0x04`) of each mipmap level's bitstream, ordered from largest (`LOD 0`) to smallest (`LOD NumLOD-1`).

#### Uncompressed Texel Bit Packing (MSB-First Bitstream)

Each texel in an uncompressed LOD (or inside a compressed non-transparent run) is packed MSB-first in the exact order:

1. **`SSB`** (`1` bit, if `HasSSB` is set).
2. **`Alpha`** (`aDepth` bits, if `HasAlpha` is set; expanded to 8-bit by repeating the MSB/bits).
3. **`Color`**:
   - If `IsLiteral` (`0x1000`): `R` (`cDepth` bits), `G` (`cDepth` bits), `B` (`cDepth` bits).
   - If Indexed (`!IsLiteral`): `Index` (`cDepth` bits, looked up in `M2PI[(Index + pip_offset) & 0xFF]`).

#### Compressed Texel Bitstream (`IsCompressed` / `M2CI`)

When `M2TX.Flags & 1` is set, each LOD is an MSB-first bitstream of variable-length packets:
1. Read an 8-bit `control` byte from the bitstream:
   - `fmt = (control >> 6) & 0x3` selects `TexelFormat[fmt]` from `M2CI`.
2. **If `TexelFormat[fmt]` has `IsTrans` (`0x0100`) set**:
   - `count = (control & 0x3F) + 1` (`1..64` pixels).
   - Emit `count` identical pixels using constant index/color `TxExpColorConst[fmt] & 0xFF` (no extra texel bits follow).
3. **Otherwise (non-transparent format)**:
   - `is_literal_run = (control >> 5) & 1`.
   - `count = (control & 0x1F) + 1` (`1..32` pixels).
   - If `is_literal_run == 1`: read `count` distinct texels from the bitstream using `TexelFormat[fmt]`.
   - If `is_literal_run == 0`: read `1` texel from the bitstream using `TexelFormat[fmt]` and repeat it `count` times.

### `M2PG` Paged Texture Directory (`.txp`)

In `.txp` files (and paged textures inside `.memimage`), the `M2PG` chunk defines `NumTex` sub-textures that share the parent `M2TD` and `M2PI`:
- `+0x00`: `u32 NumTex`, `u32 Version`.
- Followed by `NumTex` sub-headers (`24` bytes each): `{ u32 texelOffset, u32 flags, u16 minX, u16 minY, u16 texFormat, u8 numLOD, u8 pipIndexOffset (at +15), ... }`. Each sub-texture adds its `pipIndexOffset` (`byte 15`) to decoded palette indices modulo 256.

## `Message.cel` (Legacy 3DO Packed Cel)

Root file `Message.cel` (`5,292` bytes) is a standard 3DO Opera Cel containing 3 big-endian chunks decoded by [`tools/decode_message_cel.py`](../../tools/decode_message_cel.py):

1. **`CCB ` (`72` bytes)**: Cel Control Block with `ccb_Flags`, `ccb_PRE0 = 0x000011C5` (`VCNT = 71` -> height `72`, `BPP = 5` -> 8-bit coded where low 5 bits index the 32-entry `PLUT`), and `ccb_Width = 206`.
2. **`PDAT` (`5,200` bytes)**: Row-aligned packed scanline bitstream. Each scanline starts with a 10-bit word-offset-minus-2 header (in bits `25..16` of the row's first 32-bit word), followed by MSB-first packets: `2-bit type` (`00` = end of row, `01` = literal `count+1` pixels, `10` = transparent `count+1` pixels, `11` = repeat 1 pixel `count+1` times) + `6-bit count`.
3. **`PLUT` (`76` bytes)**: 32-entry big-endian RGB555 palette table.

## Usage

```bash
# Decode a standalone FORM TXTR sprite to PNG
mkdir -p converted
python3 tools/decode_txtr.py extracted/shooter2d/bee.utf -o converted/bee.png

# Decode the root Message.cel screen to PNG
python3 tools/decode_message_cel.py extracted/Message.cel -o converted/message.png
```

`decode_txtr.py` decodes standalone IFF `FORM`/`CAT` texture and font files to PNG, adding filename suffixes when an archive contains multiple forms or mip levels. To decode textures embedded in a `.memimage` bundle, slice the target `FORM TXTR` record using its export offset first.
