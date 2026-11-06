# Portfolio OS Filesystem (`folio 4`), CD-ROM Device, and `/card` Save Storage

Portfolio OS v3.0 exposes disc and nonvolatile storage through the File Folio (`module 3`, host `folio 4`) and underlying block device stacks (`CMD_STATUS`, `CMD_BLOCKREAD`).

In this repository, the Virtual Filesystem (VFS), OperaFS directory/file mapping, working-directory task inheritance, and `/card` MicroCard save storage are implemented in [`shims/`](../../shims/) and [`host/core/`](../../host/core/). For the on-disc binary layout of the 3DO OperaFS CD-ROM image, see [OperaFS Disc Image Format](../formats/operafs.md).

## Namespace Mount Topology and Working Directory Inheritance

In Portfolio OS, the root path `"/"` is a virtual mount table enumerating mounted filesystem volumes rather than the root directory of a single disc.

The host VFS exposes two synthetic root mounts (see [Folio 4: `filesystem`](../recompilation/shims.md#folio-4-filesystem)):
- `/data`: Read-only application asset root (`driving/data` for the racing game, or `shooter2d` when booting the companion game directly).
- `/card`: Writable MicroCard save directory.

### Task Working Directory Inheritance (`FileFolioCreateTaskHook`)

When `LaunchMe` spawns `driving/game` (or the game spawns its loading and soundtrack workers), Portfolio OS's `FileFolioCreateTaskHook` copies the parent task's current directory (`fftd_CurrentDirectory`) to the child task. Relative paths opened by `driving/game` (such as `"data/ui/uiart.memimage"` or `"data/tracks/testona/ra_test_trck.qmf"`) therefore resolve relative to `"driving/"`.

## Imported File Folio Selectors (`folio 4`)

`driving/game` imports 12 functions from `System.m2/Modules/filesystem`:

| Ordinal | Prototype | Behavior |
|---|---|---|
| `4:0` | `Item OpenFile(const char *path)` | Opens a file `Item` (`FILENODE`) for block-level or stream I/O (used by `DataAcqThread` for `.mov` files and `SoundPlayer` for `.cbd2` files). |
| `4:1` | `Err CloseFile(Item fileItem)` | Closes a file `Item` opened by `OpenFile`. |
| `4:10` | `Item CreateAlias(const char *aliasPath, const char *originalPath)` | Registers a path alias in the File Folio namespace. |
| `4:32` | `Directory *OpenDirectoryPath(const char *path)` | Opens a directory stream for enumeration (used in `FindMicroCard` at `0x1faa8` to scan `"/"` for mounted writable volumes). |
| `4:33` | `Err ReadDirectory(Directory *dir, DirectoryEntry *de)` | Reads the next `DirectoryEntry` (`de_Flags` at `+0x00`, `de_UniqueIdentifier` at `+0x04`, `de_Type` at `+0x08`, `de_BlockSize` at `+0x0C`, `de_ByteCount` at `+0x10`, `de_BlockCount` at `+0x14`, `de_FileName[32]` at `+0x20`). Returns `< 0` when exhausted. |
| `4:34` | `void CloseDirectory(Directory *dir)` | Closes a `Directory *` handle. |
| `4:35` | `Err OpenRawFile(RawFile **file, const char *path, FileOpenModes mode)` | Opens a buffered `RawFile *` stream (`FILEOPEN_READ = 0`, `FILEOPEN_WRITE`, `FILEOPEN_WRITE_NEW`, `FILEOPEN_READWRITE`). |
| `4:36` | `Err CloseRawFile(RawFile *file)` | Closes a `RawFile *` stream and flushes any writes. |
| `4:37` | `int32 ReadRawFile(RawFile *file, void *buffer, int32 numBytes)` | Reads up to `numBytes` into guest `buffer`; returns bytes read (`>= 0`) or a negative `Err`. |
| `4:38` | `int32 WriteRawFile(RawFile *file, const void *buffer, int32 numBytes)` | Writes `numBytes` from guest `buffer`; returns bytes written (`>= 0`) or a negative `Err`. |
| `4:39` | `int32 SeekRawFile(RawFile *file, int32 pos, FileSeekModes mode)` | Seeks within `file` (`FILESEEK_START = 1`, `FILESEEK_CURRENT = 2`, `FILESEEK_END = 3`). On success, `SeekRawFile` returns the previous byte offset before the seek (not the new position), which the IFF Folio (`folio 2`) relies on to determine file size. |
| `4:40` | `Err GetRawFileInfo(const RawFile *file, FileInfo *info, uint32 infoSize)` | Populates `FileInfo` (`fi_File` Item at `+0x00`, `fi_BlockSize = 2048` at `+0x08`, `fi_ByteCount` at `+0x0C`, `fi_BlockCount` at `+0x10`, `fi_Flags` at `+0x14`). |

## Block Device I/O (`CMD_STATUS` and `CMD_BLOCKREAD`)

When streaming `.mov` video (`DataAcqThread`) or `.cbd2` music (`SoundPlayer`), guest code opens a file with `OpenFile(path)` (`4:0`), creates an `IOReq` (`CreateIOReq` `1:170`) bound to that `fileItem`, and submits block device commands via `DoIO` (`1:132`) or `SendIO` (`1:142`):

### `CMD_STATUS` (`0x00000102`)

Writes a `DeviceStatus` structure (`FileDriverStatus`) to `ioi_Recv.iob_Buffer`:
- `ds_DriverIdentity = DI_FILE` (`0x04`)
- `ds_DeviceType = DS_DEVTYPE_FILE`
- `ds_FamilyCode = DS_DEVTYPE_FILE`
- `ds_MaximumStatusSize = sizeof(DeviceStatus)`
- `ds_DeviceBlockSize = 2048` (`0x800`)
- `ds_DeviceBlockCount = (file_size + 2047) / 2048`
- `ds_DeviceFlagWord = DS_USAGE_FILESYSTEM | DS_USAGE_READONLY`

### `CMD_BLOCKREAD` (`0x00000105`)

Reads `ioi_Recv.iob_Len` bytes (`block_count * 2048`) starting at sector index `ioi_Offset` (byte offset `ioi_Offset * 2048`) into `ioi_Recv.iob_Buffer`, setting `ior->io_Actual = ioi_Recv.iob_Len`. If the final sector read extends past the logical `de_ByteCount` of the file, trailing bytes in the 2048-byte sector are zero-padded.

## MicroCard Discovery and Persistent Save Files (`/card`)

`driving/game` discovers writable storage in `FindMicroCard` (`0x1faa8`):
1. Calls `OpenDirectoryPath("/")` (`4:32`) and iterates over mounted volumes with `ReadDirectory` (`4:33`). The host returns two root entries in mount order: `data` (read-only application assets) and `card` (`M2_CARD_VOLUME`).
2. Skips the developer host-link volume name `remote` and selects the first writable volume (`card`).
3. Reads and writes persistent files via `OpenRawFile` / `ReadRawFile` / `WriteRawFile`:
   - **High Scores & Records**: `IMSA_HiScores` (`LoadHiScores` / `SaveHiScores`).
   - **User Options & Controller Setup**: Saved via `WriteMicroCardFile` (`0x1fd30`) and loaded via `ReadMicroCardFile`.
   - **Race Replays**: Saved and loaded by `repSaveReplay` (`0x26f4c`) and `repLoadReplay`.
   - **Developer Screenshots**: `Dsply_screenshot` (`0x124e8`) writes `screenshot.%d` (a `497,664`-byte raw RGB555 image) to `M2_SCREENSHOT_ROOT` (defaulting to `screenshots` next to the platform card directory) alongside a `.ppm` preview.
