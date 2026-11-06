# `.RGB` Raw 16-Bit Still Image Format

Full-screen 576×432 bitmap splash screens under `driving/data/rgb/` (`bumper.RGB`, `credits.RGB`, `logo.RGB`, `no_device.RGB`) use a simple 12-byte header followed by uncompressed big-endian 16-bit RGB555 pixels.

The standalone converter is in [`tools/decode_rgb.py`](../../tools/decode_rgb.py).

## Binary Layout (`497,676` bytes)

| Offset | Size | Type | Value | Description |
|---|---|---|---|---|
| `+0x00` | `8` | `char[8]` | `"RAW RGB "` | 8-byte ASCII magic string (including trailing space `0x20`). |
| `+0x08` | `2` | `u16 BE` | `576` (`0x0240`) | Image width in pixels. |
| `+0x0A` | `2` | `u16 BE` | `432` (`0x01B0`) | Image height in pixels. |
| `+0x0C` | `497,664` | `u16 BE[]` | `576 * 432` pixels | Uncompressed row-major top-to-bottom, left-to-right 16-bit RGB555 framebuffer data. |

### Pixel Encoding (`16-bit Big-Endian RGB555`)

Each 16-bit word `w` encodes one pixel in M2 16-bit framebuffer format (`BMF_16BPP`):

```text
Bit:  15  14 13 12 11 10   9  8  7  6  5   4  3  2  1  0
     [SSB][     R5      ] [     G5      ] [     B5      ]
```

- **Bit 15 (`SSB`)**: Source Select Bit (`1` on all opaque pixels).
- **Bits 14..10 (`R5`)**: 5-bit Red (`0..31`), expanded to 8-bit via `(r5 << 3) | (r5 >> 2)`.
- **Bits 9..5 (`G5`)**: 5-bit Green (`0..31`), expanded to 8-bit via `(g5 << 3) | (g5 >> 2)`.
- **Bits 4..0 (`B5`)**: 5-bit Blue (`0..31`), expanded to 8-bit via `(b5 << 3) | (b5 >> 2)`.

## Headerless Screenshot Variant (`Dsply_screenshot`, `0x124e8`)

When the in-game developer screenshot function `Dsply_screenshot` (`0x124e8`) writes `/remote/screenshot.%d`, it dumps the active 576×432 16-bit framebuffer (`0x476000` or `0x4f0000`) directly via `WriteRawFile` **without** the 12-byte `"RAW RGB "` header, producing a raw `497,664`-byte (`0x79800`) big-endian RGB555 file.

## Disc Inventory and Usage

| File | Description |
|---|---|
| `driving/data/rgb/bumper.RGB` | Legal / copyright bumper screen (`576×432`). |
| `driving/data/rgb/logo.RGB` | Studio / publisher logo screen (`576×432`). |
| `driving/data/rgb/credits.RGB` | Credits background screen (`576×432`). |
| `driving/data/rgb/no_device.RGB` | Missing controller warning screen (`576×432`). |

```bash
# Convert a .RGB file to PNG or PPM
mkdir -p converted
python3 tools/decode_rgb.py extracted/driving/data/rgb/logo.RGB -o converted/logo.png
```
