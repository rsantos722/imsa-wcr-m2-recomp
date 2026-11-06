# OperaFS CD-ROM Filesystem Format

*IMSA World Championship Racing* uses a 2048-byte/sector OperaFS image. The volume header declares 327,680 logical blocks (671,088,640 bytes), while the physical ISO is 671,399,936 bytes (327,832 sectors, including 152 trailing sectors). Despite targeting M2, the disc retains the standard big-endian 3DO Opera filesystem header at LBA 0.

The disc contains 594 files across 56 directories, examined and extracted by [`tools/operafs.py`](../../tools/operafs.py).

## Volume Header (`DiscLabel`, LBA 0)

The volume label resides at sector 0 (`offset 0x0000`) and is big-endian (`>B5sB...`):

| Offset | Size | Type | Field | Value on Disc / Description |
|---|---|---|---|---|
| `+0x00` | `1` | `u8` | `dl_RecordType` | `0x01` (standard volume record) |
| `+0x01` | `5` | `char[5]` | `dl_SyncBytes` | `"ZZZZZ"` (`5A 5A 5A 5A 5A`) |
| `+0x06` | `1` | `u8` | `dl_VolumeStructureVersion` | `0x01` |
| `+0x07` | `1` | `u8` | `dl_Flags` | Volume flags |
| `+0x08` | `32` | `char[32]` | `dl_VolumeComment` | NUL-padded comment string |
| `+0x28` | `32` | `char[32]` | `dl_VolumeIdentifier` | `"CD-ROM"` (NUL-padded) |
| `+0x48` | `4` | `u32` | `dl_VolumeUniqueIdentifier` | `0x21F3B3FC` |
| `+0x4C` | `4` | `u32` | `dl_VolumeBlockSize` | `2048` (`0x00000800`) |
| `+0x50` | `4` | `u32` | `dl_VolumeBlockCount` | `327680` (`0x00050000`) |
| `+0x54` | `4` | `u32` | `dl_RootUniqueIdentifier` | `0x38F6F285` |
| `+0x58` | `4` | `u32` | `dl_RootDirectoryBlockCount` | `1` |
| `+0x5C` | `4` | `u32` | `dl_RootDirectoryBlockSize` | `2048` |
| `+0x60` | `4` | `u32` | `dl_RootDirectoryLastAvatarIndex` | `6` (7 avatar copies total, indices `0..6`) |
| `+0x64` | `28` | `u32[7]` | `dl_RootDirectoryAvatarList` | `0x00048686 .. 0x0004868C` (7 replicated root directory blocks) |
| `+0x80` | `...` | `char[]` | Filler | Repeated ASCII `"iamaduck"` to the end of sector 0 |

OperaFS replicates metadata across multiple physical LBAs called **avatars**. The parser uses `dl_RootDirectoryAvatarList[0]` (`LBA 0x48686`) as the primary root directory block.

## Directory Block Header (`DirectoryHeader`, `0x14` bytes)

Each directory is stored as a contiguous extent of `block_count` 2048-byte sectors starting at the directory's first avatar LBA. Every 2048-byte sector in a directory extent starts with a 20-byte (`0x14`) big-endian header (`>iiIII`):

| Offset | Size | Type | Field | Description |
|---|---|---|---|---|
| `+0x00` | `4` | `i32` | `dh_NextBlock` | Relative block index of the next block in this directory (`-1` / `0xFFFFFFFF` on the final block). For example, `System.m2/Audio/dsp` spans 3 blocks with `dh_NextBlock` values `1`, `2`, `-1`. |
| `+0x04` | `4` | `i32` | `dh_PrevBlock` | Relative block index of the previous block (`-1` on block 0). |
| `+0x08` | `4` | `u32` | `dh_Flags` | Directory block flags (`0`). |
| `+0x0C` | `4` | `u32` | `dh_FirstFreeByte` | Byte offset within this 2048-byte block after the last valid entry. |
| `+0x10` | `4` | `u32` | `dh_FirstEntryOffset` | Byte offset of the first `DirectoryEntry` in this block (always `0x00000014`). |

## Directory Entry (`DirectoryEntry`, `0x44 + (last_avatar + 1) * 4` bytes)

Entries are packed sequentially within a directory block starting at `dh_FirstEntryOffset` (`0x14`). Entries never straddle a 2048-byte sector boundary. Each entry has a 68-byte (`0x44`) fixed header (`>II4sIIIIII32sI`) followed by `(de_LastAvatarIndex + 1)` big-endian `u32` avatar start LBAs:

| Offset | Size | Type | Field | Description |
|---|---|---|---|---|
| `+0x00` | `4` | `u32` | `de_Flags` | Entry type and boundary flags (see below). |
| `+0x04` | `4` | `u32` | `de_UniqueIdentifier` | Item unique ID. |
| `+0x08` | `4` | `char[4]` | `de_Type` | FourCC file type tag (e.g., `'    '`, `'*dir'`, `'m2  '`, `'cel '`, `'dsp '`, `'font'`, `'utf '`, `'aifc'`, or small binary values `0..3` for `shooter2d/wavepattern.*`). |
| `+0x0C` | `4` | `u32` | `de_BlockSize` | Logical block size (`2048`). |
| `+0x10` | `4` | `u32` | `de_ByteCount` | Exact file payload length in bytes (`0` or `block_count * 2048` for directories). |
| `+0x14` | `4` | `u32` | `de_BlockCount` | Number of 2048-byte sectors allocated per avatar copy. |
| `+0x18` | `4` | `u32` | `de_Burst` | CD-ROM burst parameter. |
| `+0x1C` | `4` | `u32` | `de_Gap` | CD-ROM interleave gap parameter. |
| `+0x20` | `32` | `char[32]` | `de_FileName` | NUL-padded ASCII filename. Note: some filenames on disc contain `0x7F` (DEL) characters, which [`tools/operafs.py`](../../tools/operafs.py) sanitizes to `_`. |
| `+0x40` | `4` | `u32` | `de_LastAvatarIndex` | Highest valid index in `de_AvatarList` (`N - 1`). |
| `+0x44` | `4 * N` | `u32[N]` | `de_AvatarList` | Starting sector LBAs for each avatar copy (`N = de_LastAvatarIndex + 1`). |

### Entry Flag Bits (`de_Flags`)

| Mask / Value | Meaning |
|---|---|
| `de_Flags & 0xFF == 0x02` | Regular file (`FILE_TYPE_FILE`). M2 executables and assets often set bit `0x2000` as well (`0x00002002`). |
| `de_Flags & 0xFF == 0x06` | Special volume/system file entry. |
| `de_Flags & 0xFF == 0x07` | Subdirectory (`FILE_TYPE_DIRECTORY`). |
| `0x40000000` | `DIRECTORY_LAST_IN_BLOCK` — last entry in the current 2048-byte directory sector; advance to the next sector in the directory extent (`dh_NextBlock`). |
| `0x80000000` | `DIRECTORY_LAST_IN_DIR` — final entry in the entire directory; terminates iteration even if `DIRECTORY_LAST_IN_BLOCK` is also set. |

Files are stored as contiguous sector runs starting at byte offset `de_AvatarList[0] * 2048` (`de_ByteCount` bytes).

## Top-Level Disc Layout

| Path | Description |
|---|---|
| `LaunchMe` | Legacy ARM executable (`7,552` bytes), separate from the M2 shell. |
| `LaunchMe.m2` | M2 PowerPC ELF disc shell (`29,511` bytes); spawns `driving/game` or the companion shooter. |
| `Message.cel` | Legacy 3DO Opera packed Cel screen ("Sorry, this CD is for M2. Panasonic") displayed if inserted into an original 3DO console. |
| `System/` | Legacy 3DO Opera system tree (`System/Audio/dsp/`, fonts, drivers). |
| `System.m2/` | M2 Portfolio OS v3.0 system tree (`Boot/kernel.opera`, `Boot/oscode`, `Boot/oscode.opera`, `Modules/`, `Drivers/`, `Audio/dsp/`, `Tasks/`, `Fonts/`). |
| `driving/game` | Main racing M2 PowerPC ELF (`1,831,772` bytes), with uncompressed 483,436-byte `.text`. |
| `driving/data/` | `aiff/` sample banks and `sound.spt`; `Music/` CBD2 music/voiceovers; `tracks/` tracks and `shared/cars1..4.memimage`; `showroom/` model packs; `ui/` UI art and `temp.mpg`; `rgb/` stills; `movies/` DataStream FMVs. |
| `shooter2d/` | The companion game (`shooter2d` M2 ELF, `wavepattern.0..3`, `.utf` sprite sheets, `.aifc` sound effects) launched by the [Skid Pad shooter trigger](../game/tracks.md#skid-pad-trigger-volumes). |

Exact filenames, case, sizes, and SHA-256 digests are recorded in [`launcher/disc-manifest.json`](../../launcher/disc-manifest.json). See [`shooter2d` Runtime and Assets](../game/shooter2d.md) for its direct-launch path and assets.

## Inspection and Extraction

Use [`tools/operafs.py`](../../tools/operafs.py) to list or extract an OperaFS ISO image:

```bash
# List all 594 files and 56 directories with LBA, size, flags, and fourcc type
python3 tools/operafs.py list disc.iso

# Extract the entire filesystem tree
python3 tools/operafs.py extract disc.iso extracted/
```
