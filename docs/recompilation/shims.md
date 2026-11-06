# 3DO M2 Portfolio OS Folio Shims

On the 3DO M2, applications do not issue raw `sc` supervisor traps or link against static OS libraries. Instead, the Portfolio operating system exposes services through dynamic **folios**—modular libraries loaded by the kernel. In compiled PowerPC code, each OS call is a relative branch (`bl`) tagged with an `R_IMPREL24` (type 202) relocation encoding a `(module_index, ordinal)` selector pair.

The host and runtime implement these Portfolio folios through C/C++ shims in [`shims/`](../../shims/), [`host/core/`](../../host/core/), [`host/audio/`](../../host/audio/), and [`runtime/src/`](../../runtime/src/).

## Selector Dispatch Architecture

During translation, [`recompiler/transpile.py`](../../recompiler/transpile.py) maps each `.imp3do` module name to a host folio ID (`1`–`9`) and emits a call to `recomp_call_selector()` with the folio ID, ordinal, and guest PC.

| Folio ID | Portfolio Module | Implementation | Responsibilities |
| :--- | :--- | :--- | :--- |
| `1` | `kernel` | [`guest_kernel.c`](../../runtime/src/guest_kernel.c)<br/>[`kernel_shim.cpp`](../../host/core/kernel_shim.cpp) | Tasks, threads, semaphores, signals, message ports, heap allocation, and timers. |
| `2` | `iff` | [`recomp_runtime.c`](../../runtime/src/recomp_runtime.c) | IFF chunk stream parsing and property context stacks. |
| `3` | `graphics` | [`small_folio_shims.cpp`](../../host/core/small_folio_shims.cpp) | Display view registration and bitmap items. |
| `4` | `filesystem` | [`filesystem_shim.cpp`](../../host/core/filesystem_shim.cpp) | Virtual volume mounting (`/data`, `/card`), path resolution, and file I/O. |
| `5` | `audiopatch` | [`small_folio_shims.cpp`](../../host/core/small_folio_shims.cpp) | Audio instrument patch construction and DSP graph wiring. |
| `6` | `audio` | [`audio_shim.cpp`](../../host/audio/audio_shim.cpp)<br/>[`dsp_template_loader.c`](../../runtime/src/dsp_template_loader.c) | Voice allocation, sample playback, cues, knobs, and DSPP execution. |
| `7` | `mpegaudiodecoder` | [`mpeg_decoder.cpp`](../../host/audio/mpeg_decoder.cpp) | MPEG audio/video decoding via system FFmpeg. |
| `8` | `gstate` | [`recomp_runtime.c`](../../runtime/src/recomp_runtime.c) | Triangle Engine command list buffers (`GS_SendList`, `SetTEWritePointer`). |
| `9` | `font` | [`font_folio.c`](../../runtime/src/font_folio.c) | Font parsing, glyph layout, and text cel generation for `shooter2d`. |

In `driving/game`, 2,962 `R_IMPREL24` call sites target 214 distinct selector ordinals. Named exports are cataloged in [`shims/abi/selector-names.json`](../../shims/abi/selector-names.json).

## ABI and Calling Conventions

All folio shims follow the PowerPC EABI and keep guest memory isolated from host pointers:

- **Register Passing**: Integer and pointer arguments arrive in `c->gpr[3]`..`c->gpr[10]` (`r3`–`r10`), and floating-point arguments arrive in `c->fpr[1]`..`c->fpr[8]`. Return values are placed in `c->gpr[3]` or `c->fpr[1]`, with negative Portfolio error codes (such as `M2K_ERR_NOT_OWNER` or `M2_TE_ERR_BADITEM`) encoded as 32-bit two's-complement values.
- **Opaque Item Handles**: Guest registers and memory store 32-bit Portfolio item tokens (`type | (slot << 4) | (generation << 12)`) or guest virtual addresses rather than 64-bit host pointers.
- **Big-Endian Marshalling**: Guest structures (`TagArg`, `IOInfo`, `FileStatus`, `DirectoryEntry`) are read and written through the big-endian accessors (`rd16`/`rd32`/`wr16`/`wr32`) in [`runtime/include/ppc_ctx.h`](../../runtime/include/ppc_ctx.h).

## Implementation Notes

### Folio 1: `kernel`
Covers 156 ordinals. Each guest task runs on a native `pthread`, with 1,024-slot message pools, recursive FIFO semaphores, and microsecond metronome timers (`TIMERCMD_METRONOME_USEC`).

### Folio 2: `iff`
Handles chunk recursion up to 16 levels. The track, car, and sound loaders use its entry, exit, and purge callbacks.

### Folio 4: `filesystem`
[`host/core/filesystem_shim.cpp`](../../host/core/filesystem_shim.cpp) maps Portfolio virtual paths onto the host filesystem:
- **`/data`**: Read-only asset root (`<game-data>/driving/data` for racing, `<game-data>/shooter2d` for the companion game).
- **`/card`**: Read/write virtual 3DO MicroCard directory (`M2_MICROCARD_ROOT`) where preferences, ghost laps, and high scores (`IMSAprefs`) are persisted.
- **Path Resolution and Screenshots**: Performs case-insensitive path matching, tracks per-task working directories (`ChangeDirectory`, `GetDirectory`), and writes developer `screenshot.N` framebuffer dumps alongside `.ppm` previews.

### Folios 5, 6, and 7: `audiopatch`, `audio`, and `mpegaudiodecoder`
[`host/audio/audio_folio_shim.cpp`](../../host/audio/audio_folio_shim.cpp) implements DSP instrument templates, patch routing (`audiopatch` / `audio`), and MPEG still/video frame decoding (`mpegaudiodecoder`). For the 576×432 post-race MPEG still images, [`mpeg_still_packet.h`](../../host/audio/mpeg_still_packet.h) strips the 12-byte custom header and prepends a standard MPEG sequence header before decoding.

### DSPP Interpreter (`audio` / `audiopatch`)
Each instrument runs one cyclic pass per audio frame: the program restarts at its entry point, executes until it reaches `SLEEP`, and persists data memory, FIFO state, and I/O registers between frames. Instruction fetch, operand classification, `RBASE` translation, and indirect reads are performed inline per executed instruction.

One FDMA channel is serviced before each instruction. The interpreter maintains a 32-bit bitmask of channels that are enabled and not complete, probing from highest channel (`31`) down to `0` to match hardware priority. Every DMA transition that updates `enabled` or `complete` refreshes the bitmask, and the service path revalidates channel flags before transferring data.

The per-DAC-frame mix pass uses a structural index rebuilt whenever the audio graph changes (item creation/deletion, connections, instrument start/stop, or patch registration). The index caches `line_out.dsp` `Input` routes, source node kinds, resolved `OUTPUT` words, and active `WITH_LINE_OUT` mixers so per-frame mixing requires no pool scans or string comparisons. Nanokernel staging addresses (`gMixer`, `Amplitude`) are cached per export-table revision.
