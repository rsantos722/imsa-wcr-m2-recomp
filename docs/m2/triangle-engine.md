# M2 Bulldog Triangle Engine (TE), Command List Toolkit (CLT), and Graphics Folio

The 3DO M2 **Bulldog ASIC** replaces the original 3DO's quad-warping Cel engine with a dedicated 3D hardware rasterizer: the **Triangle Engine (TE)**. All 2D UI layers and 3D racing scenes in *IMSA World Championship Racing* are rendered by building hardware command lists through the **Command List Toolkit (CLT)** and Mercury (`M_Draw`) libraries, submitting them via the `triangleengine` device (`CMDC_TE = 0x000D`), and presenting them through the Graphics Folio (`folio 3`).

## Graphics Folio (`folio 3`) and Display View Configuration

The game allocates double-buffered 576 × 432 16-bit RGB555 (`BMF_16BPP`) framebuffers and a shared 16-bit Z-buffer on the guest heap:

| Surface | Typical Guest Heap Address | Dimensions | Stride | Byte Size |
|---|---|---|---|---|
| **Framebuffer 0** | `0x00476000` | `576 × 432` | `1,152` B | `497,664` B (`0x79800`) |
| **Framebuffer 1** | `0x004F0000` | `576 × 432` | `1,152` B | `497,664` B (`0x79800`) |
| **Z-Buffer (16-bit)** | `0x0056A000` | `576 × 432` | `1,152` B | `497,664` B (`0x79800`) |

The 576×432 `View` Item is centered inside the 640×480 NTSC output at screen offset `(32, 24)` (`VIEW_TAG_LEFTEDGE = 32`, `VIEW_TAG_TOPEDGE = 24`) and attached or detached via:
- `AddViewToViewList(viewItem, viewListItem)` (`3:3`)
- `RemoveView(viewItem)` (`3:5`)

## Triangle Engine Device (`CMDC_TE = 0x000D`) and Ring Pointers

### Device Commands (`ioi_Command`)

| Command Name | Code | Behavior |
|---|---|---|
| `TE_CMD_EXECUTELIST` | `0x000D0003` | Executes the CLT command buffer at `ioi_Send.iob_Buffer` (`ioi_Send.iob_Len` bytes) and/or performs hardware buffer clears specified in `ioi_CmdOptions` (mask `0x1F`). |
| `TE_CMD_SETVIEW` | `0x000D0004` | Binds the output `View` Item in `ioi_CmdOptions` for VBlank/offscreen synchronization. |
| `TE_CMD_SETZBUFFER` | `0x000D0005` | Binds the active Z-buffer `Bitmap` Item (or raw address) in `ioi_CmdOptions`. |
| `TE_CMD_SETFRAMEBUFFER` | `0x000D0006` | Binds the render-target `Bitmap` Item in `ioi_CmdOptions`. |
| `TE_CMD_DISPLAYFRAMEBUFFER` | `0x000D0007` | Latches the rendered framebuffer for display on the bound `View` at the next VBlank (`ModifyGraphicsItemVA(view, VIEW_TAG_BITMAP, fb)`). |
| `TE_CMD_STEP` | `0x000D0008` | Single-steps the TE instruction pointer (debug). |
| `TE_CMD_SPEEDCONTROL` | `0x000D0009` | Adjusts TE DMA priority. |
| `TE_CMD_SETVBLABORTCOUNT` | `0x000D000A` | Sets the number of VBlanks before aborting an overrunning command list. |
| `TE_CMD_GETIDLETIME` | `0x000D0100` | Writes 32-bit idle counter to `ioi_Recv.iob_Buffer`. |

#### `TE_CMD_EXECUTELIST` Option Flags (`ioi_CmdOptions & 0x1F`)

| Flag | Mask | Behavior |
|---|---|---|
| `TE_CLEAR_FRAME_BUFFER` | `0x00000001` | Clears the bound framebuffer using the clear command list at `ioi_Offset`. |
| `TE_CLEAR_Z_BUFFER` | `0x00000002` | Clears the bound 16-bit Z-buffer using `ioi_Offset`. In Portfolio's `tedriver`, if both `TE_CLEAR_FRAME_BUFFER` and `TE_CLEAR_Z_BUFFER` are set in the same `SendIO`, `TE_CLEAR_Z_BUFFER` takes precedence for the hardware clear list, which is why `GS_ClearFBZ` issues two separate `SendIO` calls when clearing both buffers. |
| `TE_WAIT_UNTIL_OFFSCREEN` | `0x00000004` | Waits until the bound render framebuffer is no longer being scanned out before executing. |
| `TE_ABORT_AT_VBLANK` | `0x00000008` | Aborts command list execution if the VBL abort threshold is reached (`GS_SetAbortVblank`). |
| `TE_LIST_FLUSHED` | `0x00000010` | Indicates the caller has already flushed CPU data cache lines covering the command list. |

### Direct Ring Pointers (`GetTEReadPointer` and `SetTEWritePointer`)

In addition to `TE_CMD_EXECUTELIST`, the GState (`GS_*`) layer manages segmented command buffers using two Kernel Folio supervisor hooks:
- `GetTEReadPointer()` (`1:558`): Reads the hardware Instruction Read Pointer (`IRP`). `GS_WaitTEPass` masks the returned pointer with `0x3FFFFFFF` to strip the top 2 hardware status bits before comparing it against the command list cursor.
- `SetTEWritePointer(uint32_t wp)` (`1:561`): Advances the hardware Instruction Write Pointer (`IWP`), executing any newly appended CLT commands between the previous write cursor and `wp`.

## Command List Toolkit (CLT) Instruction Format

A CLT command list is a big-endian stream of 32-bit words interpreted by the Triangle Engine's front-end parser:

### Register Write Header (`0x10XXXXXX`)

```text
Bits 31..28:  0x1 (register-write opcode)
Bits 23..16:  Count - 1 (number of 32-bit register values following this header)
Bits 15..0:   Triangle Engine register index (16-bit RA_* value, e.g. 0x8008 = DBUSERCONTROL);
              bits 12..11 carry the set/clear operation (0x1000/0x1800) and are masked with 0xE7FF
```

Immediately following the header word are `Count` 32-bit words written sequentially to `register_index + 0`, `+ 4`, ..., `+ (Count - 1) * 4` (see [Host CLT Interpreter](../recompilation/rendering.md#triangle-engine-command-list-interpreter-guestcltinterpreter)).

#### Special Control Registers (`DCNTL`, index `0x0014`)

- **`DCNTL` (`0x0014`)** flow and action bits:
  - `TLD` (`0x00000001`): Triggers an immediate MMDMA texture load from system RAM (`TXTLDSRCADDR`) into on-chip Texture RAM (`TRAM`).
  - `JA` (`0x00000002`): Absolute jump; `DCNTLDATA` (`0x0010`) supplies the target guest EA.
  - `JR` (`0x00000004`): Relative jump; `DCNTLDATA` supplies a signed displacement from the jump instruction.
  - `DINT` (`0x00000008`): External CPU interrupt using `DCNTLDATA` as vector data.
  - `PSE` (`0x00000010`): Pause/stop at the end of the list.
  - `SYNC` (`0x00000020`): Flushes the rasterizer pipeline before changing framebuffer or texture state.

### Triangle Strip / Fan Vertex Packet Header (`0x2XXXXXXX`)

When the top nibble is `0x2` (`(header & 0xF0000000) == 0x20000000`), the word is a Triangle Engine Vertex Packet Header:

| Bits | Mask | Meaning |
|---|---|---|
| `bits 15..0` | `0x0000FFFF` | `Count - 1` (`N = (header & 0xFFFF) + 1` vertices follow in this packet). |
| `bit 16` | `0x00010000` | `SHADE` — Per-vertex Gouraud `R, G, B, A` floats (`4` IEEE-754 floats) are present in each vertex. |
| `bit 17` | `0x00020000` | `TEXTURE` — Per-vertex `U, V` texture coordinates (`2` floats) are present in each vertex. |
| `bit 18` | `0x00040000` | `PERSPECTIVE` — Per-vertex homogeneous `1/W` (`1` float) is present in each vertex. |
| `bit 19` | `0x00080000` | `NEW` (`0` = continue existing strip/fan; `1` = start a new primitive). |
| `bit 20` | `0x00100000` | `FAN` (`0` = strip, `1` = fan). |

#### Per-Vertex Float Word Order

Each of the `N` vertices in the packet consists of `2` to `9` big-endian IEEE-754 `float` words in this fixed order:

1. `X`, `Y` (`2` floats — screen-space coordinates in pixels).
2. `R`, `G`, `B`, `A` (`4` floats, if `SHADE` `0x00010000` is set).
3. `1/W` (`1` float, if `PERSPECTIVE` `0x00040000` is set — homogeneous $1/z$; hardware depth $Z_{16}$ is derived from `W` and `DBZOFFSET`).
4. `U`, `V` (`2` floats, if `TEXTURE` `0x00020000` is set — pre-multiplied by $W$ when perspective-correct texturing is active, i.e., $u/z, v/z$).

## CLT Register Indices and Address Spaces

The tables below list `RA_*` **CLT packet indices** from the public [`cltmacros.h`](https://github.com/trapexit/portfolio_os_m2/blob/master/ws_root/src/includes/graphics/clt/cltmacros.h) header. `ESCNTL` at index `0x4000` controls edge and perspective state (bit 2 disables perspective correction).

### Texture Unit, TRAM, and PIP Palette

| CLT Index | Symbol | Description |
|---|---|---|
| `0x6408` | `TXTADDRCNTL` | Bit 13 enables texturing; bits 12..10 minification, 9..7 inter-LOD filtering, 6..4 magnification, and 3..0 maximum LOD (`0` = point, `2` = bilinear; inter-LOD `1` = linear, `3` = trilinear). |
| `0x640c` | `TXTPIPCNTL` | Bits 7..0: `pip_index_offset` (`0..255`). Bits 10..8: `pip_color_select`, Bits 13..11: `pip_alpha_select`, Bits 16..14: `pip_ssb_select` (`0` = constant from `TXTCONST0`, `1` = expanded texel value, `2` = lookup in `TEPIPRAM`). |
| `0x6410` | `TXTTABCNTL` | Texture combiner / blend mode (`PRERGB`, `PREALPHA`, `PRERGB2` modulating texture with Gouraud vertex shade or `TXTCONST`). |
| `0x6414..0x6420` | `TXTLODBASE0..3` | Byte/bit offsets in `16 KB` on-chip TRAM for mipmap levels `0..3` (and subsequent LODs derived by halving dimensions). |
| `0x6404` | `TXTLDCNTL` | MMDMA transfer mode (`Compressed`, load mode, bit/byte stride). |
| `0x6424` | `TXTLDSRCADDR` | Guest RAM source pointer for `DCNTL = TLD` texture DMA into TRAM. |
| `0x6428` | `TXTCOUNT` | Byte/bit transfer count for TRAM load. |
| `0x6434`, `0x6438` | `TXTSRCTYPE01`, `TXTSRCTYPE23` | Four 16-bit DCI `TexFormat` descriptors for hardware-decompressed `M2CI` textures. |
| `0x643c` | `TXTEXPTYPE` | Uncompressed texel format descriptor (`cDepth`, `aDepth`, `HasSSB`, `HasColor`, `HasAlpha`, `IsLiteral`). |
| `0x6440..0x644c` | `TXTCONST0..3` | Four `[SSB:1][A:7][R:8][G:8][B:8]` texture expansion / environment constants (`TxExpColorConst[0..3]`). |
| `0x642c` | `TXTUVMAX` | `(u_max << 16) \| v_max` (`(width - 1) << 16 \| (height - 1)` at `LOD 0`). |
| `0x6430` | `TXTUVMASK` | `(u_mask << 16) \| v_mask` for texture wrap (`0xFFFF`) vs. clamp (`0x0000`). |
| PIP aperture `0x46000..0x463fc` | `TEPIPRAM` | 256-entry (`1 KB`) Pen Index Palette RAM; each 32-bit entry is `[SSB:1][A:7][R:8][G:8][B:8]`. |

#### `TXTEXPTYPE` / `M2TX` Formats

`TXTEXPTYPE` specifies whether texels are palette-indexed or literal, along with their color depth, alpha depth, and Source Select Bit (`SSB`). In `driving/game`, the Mercury renderer (`M_Draw.c`) uses 6 static `TXTEXPTYPE` words for uncompressed textures:
- `0x0404`: 4-bit indexed
- `0x0E78`: 8-bit indexed + 7-bit alpha + SSB
- `0x1200`: SSB only
- `0x1605`: 16-bit literal RGB555 + SSB
- `0x1A70`: 7-bit alpha + SSB
- `0x1E78`: 32-bit literal RGBA + SSB

### Destination Blender, Clipping, and Depth

| CLT Index | Symbol | Description |
|---|---|---|
| `0x8008` | `DBUSERCONTROL` | Bits 10/9 enable Z test/output, 8/7 inside/outside window clip, 6 blending, 5 source read, and 4 dithering; bits 3..0 select alpha/R/G/B output channels. |
| `0x800c` | `DBDISCARDCONTROL` | Pixel discard conditions: `ZClipped`, `SSB0`, `RGB0`, `Alpha0`. Used for color-keyed UI sprites and cutout foliage (trees/fences). |
| `0x8010` | `DBSTATUS` | Rasterizer status word. |
| `0x801c` | `DBXWINCLIP` | `(x_min << 16) \| x_max` window clip rectangle. |
| `0x8020` | `DBYWINCLIP` | `(y_min << 16) \| y_max` window clip rectangle. |
| `0x8018` | `DBFBCLIP` | `(width << 16) \| height` destination surface guard bounds (`576 × 432`). |
| `0x8040` | `DBZCNTL` | Six independent control bits: bits 5/4 update Z/output pixel on greater Z; bits 3/2 on equal Z; bits 1/0 on smaller Z. |
| `0x8048` | `DBZOFFSET` | `(z_x_offset << 16) \| z_y_offset` relative to the active Z-buffer base. |
| `0x8030` | `DBSRCCNTL` | Source framebuffer read format (`16-bit RGB555` vs `32-bit`); 5-bit channels expand to 8-bit via `(v << 3) \| (v >> 2)` before blending. |
| `0x8034` | `DBSRCBASEADDR` | Guest address of the source framebuffer used during alpha/fog blending (`0x00476000` or `0x004F0000`). |
| `0x8038` | `DBSRCXSTRIDE` | Source stride in pixels (`576`). |
| `0x803c` | `DBSRCOFFSET` | `(src_x_offset << 16) \| src_y_offset`. |
| `0x8054` | `DBCONSTIN` | Constant RGB input `[R:8][G:8][B:8]` to the Destination Blender (used for distance fog color or flat-shaded overlays). |
| `0x8058..0x806c` | `DBAMULTCNTL`, `DBAMULTCONSTSSB0/1`, `DBBMULTCNTL`, `DBBMULTCONSTSSB0/1` | Selects multiplier coefficients for Stream A (incoming fragment or `DBCONSTIN`) and Stream B (framebuffer destination pixel): source alpha, complement of source alpha, destination alpha, or constant factors. |
| `0x8070` | `DBALUCNTL` | Blender ALU operation (`A + B` clamped, `A - B`, bitwise ops). |
| `0x8080`, `0x8084` | `DBDITHERMATRIXA`, `DBDITHERMATRIXB` | 4×4 ordered dither matrix thresholds applied when truncating 24-bit blended RGB back to 15-bit RGB555. |
