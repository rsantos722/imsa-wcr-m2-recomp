# Boot Containers, 3DO/M2 ELF Modules, and LZSS Compression

The disc contains two distinct classes of executable and system binary containers under `System.m2/` and the game directories:

1. **Legacy 3DO Opera ARM AIF/Module images** (`System.m2/Boot/kernel.opera`, `System.m2/Boot/oscode.opera`, and root `LaunchMe`) used during early CD-ROM bootstrap and wrong-console detection.
2. **3DO M2 PowerPC ELF executables and loadable modules** (`System.m2/Boot/oscode`, `LaunchMe.m2`, `driving/game`, `shooter2d/shooter2d`, and `System.m2/Modules/*`) using custom ELF machine type `0x11`. Some OS module sections use LZSS compression (`SHF_COMPRESS = 0x8`), whereas the three translated game executables are stored uncompressed.

## `kernel.opera` and `oscode.opera` (3DO ARM AIF / Sherry Header)

`System.m2/Boot/kernel.opera` (`45,892` bytes, SHA-256 `9398112ef212d73d793ad7fe7ca0f2146714c7657fba4502827795c8e9ab6ac9`) and `System.m2/Boot/oscode.opera` are uncompressed 32-bit big-endian ARM binaries formatted with a 3DO AIF + `_3DOBinHeader` prefix. They can be inspected with [`tools/decode_kernel_opera.py`](../../tools/decode_kernel_opera.py).

| Offset | Size | Value (`kernel.opera`) | Meaning |
| --- | --- | --- | --- |
| `+0x00` | `0x40` | `eb00001c eb000051 ea000010 ...` | Standard ARM AIF header (`BL` decompression/relocation/zero-init entry vectors; `+0x20 = 0x00000a6c`, `+0x2c = 0x40000000` load base). |
| `+0x40` | `0x70` | `e1a00000 ...` | ARM AIF zero-init / entry stub (`mov r0, r0` NOPs followed by BSS clear loop). |
| `+0xB0` | `4` | `0x0000B304` | Payload size after the `0x40`-byte AIF header (`45,892 - 64 = 45,828` bytes). |
| `+0xC0` | `32` | `"Sherry\0..."` | 3DO Opera kernel item name (`_3DOBinHeader.n_Name = "Sherry"`). |
| `+0xE0` | `4` | `0x041306FB` | Packed timestamp / build identifier. |
| `+0x110` | `...` | ARM machine code | Uncompressed ARM code and embedded error/diagnostic strings (`"check memory failed\n"`, `"No memory avail; limit=%lx\n"`). |

## `System.m2/Boot/oscode` (M2 PowerPC Kernel Container)

`System.m2/Boot/oscode` wraps a big-endian 32-bit PowerPC ELF (`e_machine = 0x11`) behind a 16-byte boot prefix (the `\x7fELF` magic appears at file offset `+0x10`). It contains two `PT_LOAD` segments linked at virtual address `0x00000000` and defines the M2 Portfolio OS Kernel Folio (`KernelSwiVectors`, with `f_MaxSwiFunctions` at byte offset `38` and `f_SwiFunctions` at byte offset `48` of the 68-byte `Folio` base structure).

## 3DO M2 PowerPC ELF Format (`e_machine = 0x11`)

The game executables (`driving/game`, `LaunchMe.m2`, `shooter2d/shooter2d`) and OS modules use 32-bit big-endian ELF with Diab/3DO extensions:

- **ELF Header**: `EI_CLASS = ELFCLASS32 (1)`, `EI_DATA = ELFDATA2MSB (2)`, `e_machine = 0x0011`. The three translated executables are `ET_REL (1)`, while the prefixed `oscode` ELF is `ET_DYN (3)`.
- **Compressed Section Flag (`SHF_COMPRESS = 0x00000008`)**: Standard ELF sections (`.text`, `.data`, `.rodata`) that have bit 3 (`0x8`) set in `sh_flags` store an LZSS-compressed bitstream in the file. Their uncompressed size is defined by the corresponding `PT_LOAD` program header (`p_memsz`) or section relocation bounds.
- **3DO Custom Sections**:
  - `.hdr3do`: `_3DOBinHeader` metadata, including stack and version information (`0x90` bytes in the three translated executables). Their ELF startup entries are `.text` offsets: `0x74134` for `driving/game`, `0x6b4` for `LaunchMe.m2`, and `0x122c4` for `shooter2d`.
  - `.imp3do`: Imported module table listing required dynamic Folio/Module IDs (e.g., `1` = `kernel`, `3` = `filesystem`, `9` = `iff`, `10` = `audio`, `14` = `graphics`, `22` = `audiopatch`, `23` = `mpegaudiodecoder`).
  - `.exp3do`: Exported function/data ordinal table for shared modules (`System.m2/Modules/*`).
  - Relocation sections contain `Elf32_Rela` records (12 bytes: `r_offset`, `r_info`, `r_addend`).

## Portfolio M2 LZSS Section Compression (`tools/m2_lzss.py`)

When an ELF section has `sh_flags & 0x8`, the M2 kernel loader (`FeedDecompressor` in Portfolio OS) decompresses the section payload using a byte-aligned variant of LZSS. The standalone reference implementation is in [`tools/m2_lzss.py`](../../tools/m2_lzss.py).

### LZSS Parameters

| Constant | Value | Meaning |
| --- | --- | --- |
| `INDEX_BIT_COUNT` | `12` | 12-bit window offset (`0..4095`) |
| `LENGTH_BIT_COUNT` | `4` | 4-bit match length field (`0..15`) |
| `WINDOW_SIZE` | `4096` (`0x1000`) | Circular ring buffer initialized to `0x00`, write cursor starts at `pos = 0` |
| `BREAK_EVEN` | `2` | Actual match copy length is `(mw & 0xF) + 2 + 1 = (mw & 0xF) + 3` bytes (`3..18` bytes) |
| `END_OF_STREAM` | `0` | Match offset `mpos == 0` terminates decompression |

### Bitstream State Machine

The compressed stream is organized into groups controlled by a single flag byte `flags`:

1. Read one control byte `flags`.
2. **Process 8 operations MSB-first (`mask = 0x80; mask != 0; mask >>= 1`)**:
   - If `(flags & mask) != 0`: read 1 literal byte `b`, append `b` to output, write `window[pos] = b`, and advance `pos = (pos + 1) & 0xFFF`.
   - If `(flags & mask) == 0`: read a 16-bit big-endian match word `mw = (hi << 8) | lo`. Extract `mpos = mw >> 4` and `mlen = (mw & 0xF) + 2`. If `mpos == 0`, terminate decompression. Otherwise, for `i = 0 .. mlen` inclusive (`mlen + 1` bytes total), copy `b = window[(mpos + i) & 0xFFF]` to output and write `window[pos] = b`, advancing `pos = (pos + 1) & 0xFFF` after each byte.
3. Repeat from step 1 until `mpos == 0` is encountered or the input slice is exhausted.

### `driving/game` Section Layout

| Section | File Offset | Memory Size | `sh_flags` | Storage |
| --- | --- | --- | --- | --- |
| `.text` | `0x370` | `483,436` B | `0x06` | Uncompressed code and literal pools |
| `.data` | `0x763e0` | `45,608` B | `0x03` | Uncompressed initialized data |
| `.bss` | — | `40,696` B | `0x03` | `SHT_NOBITS`; zero-filled at load |
