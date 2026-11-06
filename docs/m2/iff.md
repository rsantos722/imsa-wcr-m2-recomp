# Portfolio IFF Parser Folio

Portfolio's IFF Folio (`System.m2/Modules/iff`, module ID 9; host folio 2) provides scoped chunk parsing, property buffering, and entry/exit callbacks for [AIFF-C/CBD2](../formats/cbd2.md), [Mercury textures and fonts](../formats/memimage.md), and [DSP templates](dsp-audio.md#dsp-instrument-template-container-form-3ins--form-dspp). For how import relocations map to host folio selectors, see [Selector Dispatch Architecture](../recompilation/shims.md#selector-dispatch-architecture).

The public interface is defined in `src/includes/misc/iff.h`, `src/includes/kernel/nodes.h`, and `src/folios/iff/iff.c` in the [Portfolio OS source archive](https://github.com/trapexit/portfolio_os_m2), and implemented on the host as described in [Folio 2: `iff`](../recompilation/shims.md#folio-2-iff).

## Container Framing

A standard IFF chunk begins with a four-byte ASCII ID and a big-endian `u32` payload size, followed by that many payload bytes. The size excludes the 8-byte chunk header and any trailing alignment pad byte. `FORM`, `LIST`, `CAT `, and `PROP` containers start their payload with a four-byte form type followed by nested sub-chunks. Odd-sized leaf chunks are padded with a trailing byte to maintain 16-bit alignment, while container chunks must have even byte sizes.

Portfolio extends the 32-bit size field with reserved encodings at `0xfffffff0` and above: `0xfffffffd` indicates that an explicit 64-bit size follows the header, while `0xffffffff` and `0xfffffffe` are writer-mode sentinels requesting automatic 32-bit or 64-bit size backpatching.

## Selectors and Callbacks

| Ordinal | Function | Prototype & Behavior |
| --- | --- | --- |
| `#1` | `DeleteIFFParser` | `Err DeleteIFFParser(IFFParser *iff)` — unwinds the context stack, closes the owned stream, and frees the parser. |
| `#2` | `ParseIFF` | `Err ParseIFF(IFFParser *iff, int32 control)` — steps the chunk state machine (`IFF_PARSE_SCAN = 0`, `IFF_PARSE_STEP = 1`, `IFF_PARSE_RAWSTEP = 2`). |
| `#3` | `ReadChunk` | `int32 ReadChunk(IFFParser *iff, void *buf, uint32 numBytes)` — reads up to the remaining bytes in the active leaf chunk. |
| `#7` | `RegisterPropChunks` | `Err RegisterPropChunks(IFFParser *iff, const IFFTypeID *typeids)` — registers `{type, id}` pairs to buffer into `ContextInfo` (`IFF_CI_PROPCHUNK = 'prop'`) on chunk entry. |
| `#10` | `FindPropChunk` | `PropChunk *FindPropChunk(const IFFParser *iff, PackedID type, PackedID id)` — searches the active context stack for a buffered property chunk. |
| `#13` | `GetCurrentContext` | `ContextNode *GetCurrentContext(const IFFParser *iff)` — returns the top `ContextNode *` on the parser stack. |
| `#15` | `AllocContextInfo` | `ContextInfo *AllocContextInfo(PackedID type, PackedID id, PackedID ident, uint32 dataSize, IFFCallBack purgeCB)` — allocates a `ContextInfo` node with an inline data buffer at `+0x24`. |
| `#16` | `FreeContextInfo` | `void FreeContextInfo(ContextInfo *ci)` |
| `#17` | `FindContextInfo` | `ContextInfo *FindContextInfo(const IFFParser *iff, PackedID type, PackedID id, PackedID ident)` |
| `#18` | `StoreContextInfo` | `Err StoreContextInfo(IFFParser *iff, ContextInfo *ci, ContextInfoLocation pos)` |
| `#20` | `InstallEntryHandler` | `Err InstallEntryHandler(IFFParser *iff, PackedID type, PackedID id, ContextInfoLocation pos, IFFCallBack cb, const void *userData)` |
| `#21` | `InstallExitHandler` | `Err InstallExitHandler(IFFParser *iff, PackedID type, PackedID id, ContextInfoLocation pos, IFFCallBack cb, const void *userData)` |
| `#22` | `SeekChunk` | `Err SeekChunk(IFFParser *iff, int32 offset, IFFSeekModes mode)` |
| `#23` | `GetIFFOffset` | `int32 GetIFFOffset(IFFParser *iff)` |
| `#24` | `CreateIFFParserVA` | `Err CreateIFFParserVA(IFFParser **iff, bool writeMode, uint32 tag, ...)` — accepts `IFF_TAG_FILE` (`10`), `IFF_TAG_IOFUNCS` (`11`), and `IFF_TAG_IOFUNCS_DATA` (`12`). |

`IFFCallBack` has signature `Err (*)(IFFParser *, void *userData)`. Entry and exit handlers run at context transitions. Property registration buffers matching chunks into context nodes with identity `'prop'`, which `FindPropChunk` searches from innermost to outermost scope. Storage locations are `IFF_CIL_BOTTOM = 0`, `IFF_CIL_TOP = 1`, and `IFF_CIL_PROP = 2` (the topmost `FORM` or `LIST`). `FreeContextInfo` releases storage (or invokes the installed purge callback), whereas `RemoveContextInfo` (`#25`) only unlinks the node.

In `ParseIFF`, `IFF_PARSE_SCAN = 0` runs registered handlers until a handler returns non-zero or the stream ends, while `IFF_PARSE_STEP = 1` and `IFF_PARSE_RAWSTEP = 2` return to the caller at each context transition. Both `IFF_PARSE_EOF` and `IFF_PARSE_EOC` are negative status codes that indicate normal parser completion rather than errors.

## Guest Structures

The guest uses 32-bit big-endian structures compiled with `NO_64_BIT_SCALARS`, where 64-bit sizes and offsets are stored as `{hi, lo}` `u32` pairs and `MinNode` is 8 bytes (`next`, `prev`).

### `ContextNode` (52 bytes)

| Offset | Type | Field |
| --- | --- | --- |
| `+0x00`, `+0x04` | guest pointers | `MinNode` linkage (`n_Next`, `n_Prev`) |
| `+0x08` | `PackedID` | `cn_Type` |
| `+0x0c` | `PackedID` | `cn_ID` |
| `+0x10`, `+0x14` | `u32`, `u32` | `cn_SizeHi`, `cn_Size` |
| `+0x18`, `+0x1c` | `u32`, `u32` | `cn_OffsetHi`, `cn_Offset` |
| `+0x20` | 12-byte `List` | `cn_ContextInfo` |
| `+0x2c`, `+0x30` | `u32`, `u32` | `cn_CurrentSizeHi`, `cn_CurrentSize` |

### `ContextInfo` (36 bytes) and `PropChunk` (12 bytes)

| `ContextInfo` Offset | Type | Field |
| --- | --- | --- |
| `+0x00`, `+0x04` | guest pointers | `MinNode` linkage |
| `+0x08`, `+0x0c` | `u32`, `u32` | `ci_DataSizeHi`, `ci_DataSize` |
| `+0x10` | guest pointer | `ci_Data` (normally `ci + 0x24`) |
| `+0x14`, `+0x18`, `+0x1c` | `PackedID` | `ci_ID`, `ci_Type`, `ci_Ident` |
| `+0x20` | guest function pointer | `ci_PurgeCB` |

For buffered property chunks, `ci_Data` points to a 12-byte `PropChunk` header (`pc_DataSizeHi` at `+0x00`, `pc_DataSize` at `+0x04`, and `pc_Data` pointer at `+0x08`). The raw chunk payload bytes follow immediately at `ci + 0x30` (`pc_Data`). See `gk_iff_sync_node`, `gk_iff_alloc_info`, and `gk_iff_handle_property` in [`recomp_runtime.c`](../../runtime/src/recomp_runtime.c).

## Results, Errors, and Stream Invariants

| Symbol | 32-bit Code | Meaning |
| --- | --- | --- |
| `IFF_PARSE_EOF` | `0xd56d410a` | Logical end of stream |
| `IFF_PARSE_EOC` | `0xd56d410b` | Leaving current context |
| `IFF_ERR_NOMEM` | `0xd56d5006` | Allocation failure |
| `IFF_ERR_BADPTR` | `0xd56d5009` | Invalid parser or pointer |
| `IFF_ERR_NOTSUPPORTED` | `0xd56d5010` | Unsupported operation |
| `IFF_ERR_PREMATURE_EOF` | `0xd56d5102` | Stream ended before chunk boundary |
| `IFF_ERR_MANGLED` | `0xd56d5103` | Corrupt framing (such as an odd container size) |
| `IFF_ERR_SYNTAX` | `0xd56d5104` | Invalid chunk syntax |
| `IFF_ERR_NOTIFF` | `0xd56d5105` | Invalid top-level IFF container |

When backed by a `RawFile`, the IFF stream adapter relies on [`SeekRawFile`](operafs.md#imported-file-folio-selectors-folio-4) returning the previous stream offset on success to determine total stream length.
