# `.mov` M2 DataStream / MPEG Multiplexed Stream Format

Despite the `.mov` extension, the 10 video files under `driving/data/movies/` are not Apple QuickTime containers. They are big-endian **3DO M2 DataStream (`DS`) multiplexed chunk streams** containing interleaved MPEG-1 Video (`'MPVD'`) and MPEG-1 Layer II Audio (`'MPAU'`) subscriber packets.

In addition, `driving/data/ui/temp.mpg` is a raw elementary MPEG-1 video stream (`00 00 01 B3 ...`) without DataStream chunk framing, and `driving/data/ui/uiart.memimage` embeds 16 single-frame MPEG-1 intra still pictures (`MPS`).

## Stream Header (`SHDR`, Offset `0x00 .. 0xF3`)

Every `.mov` file begins with a 244-byte (`0xF4`) `SHDR` stream header read by `FindAndLoadStreamHeader` during `VStreamInit`:

| Offset | Size | Type | Value / Description |
|---|---|---|---|
| `+0x00` | `4` | `char[4]` | `'SHDR'` (`0x53484452`) chunk type |
| `+0x04` | `4` | `u32` | `0x000000F4` (`244` bytes total chunk size) |
| `+0x08` | `4` | `u32` | Stream header version / time |
| `+0x0C` | `16` | `u32[4]` | Stream buffer size, block size (`2048` / sector-aligned), audio clock channel, and subscriber preroll parameters |
| `+0x74` | `12` | `u32[3]` | Initial subscriber descriptor (`'MPVD'`, `0xFFFFFFFE`) copied into stream context `+0xA0` |
| `+0xF4` | `...` | Chunks | Start of multiplexed subscriber chunks (`'MPAU'`, `'MPVD'`, `'STRM'`, `'FILL'`) |

## Subscriber Chunk Framing (`SubsChunkHeader`)

Following `SHDR` at `+0xF4`, the stream consists of sequential big-endian chunks aligned to 4-byte boundaries (and padded to 2048-byte sector boundaries with `'FILL'` chunks). Standard subscriber chunks use a 20-byte (`0x14`) or 24-byte (`0x18`) header (`dsstreamdefs.h`):

| Offset | Size | Type | Field | Description |
|---|---|---|---|---|
| `+0x00` | `4` | `char[4]` | `chunkType` | Subscriber FourCC: `'MPVD'` (MPEG video), `'MPAU'` (MPEG audio), `'STRM'` (control: `'GOTO'`, `'STOP'`, `'SYNC'`), or `'FILL'` (sector padding). |
| `+0x04` | `4` | `u32` | `chunkSize` | Total byte length of this chunk including the header. |
| `+0x08` | `4` | `u32` | `time` | Presentation timestamp (PTS) in DataStream audio-clock ticks (`~240` ticks/sec; converted to 90 kHz MPEG PTS or audio frame offsets). |
| `+0x0C` | `4` | `u32` | `channel` | Logical subscriber channel number (`0` or `1`). |
| `+0x10` | `4` | `char[4]` | `subChunkType` | Subtype FourCC (see below). |

### Video Chunk Subtypes (`'MPVD'`)

- `'VHDR'` (size `0x20`): Video stream header specifying frame rate, dimensions, and decoder parameters.
- `'FRAM'` / data chunks: Contains a 32-bit PTS/flags word followed by contiguous slices of an elementary MPEG-1 video bitstream (`00 00 01 B3` sequence header, `00 00 01 B8` GOP header, `00 00 01 00` picture header, and `00 00 01 01..AF` macroblock slices).

### Audio Chunk Subtypes (`'MPAU'`)

- `'MHDR'` (size `0x1C`): MPEG audio stream header (`u32` version/channel configuration).
- `'1FRM'`, `'2FRM'`, `'3FRM'`, `'4FRM'`: Contains 1 to 4 whole MPEG-1 Layer II (`44.1 kHz` stereo) compressed audio frames.
- Split-frame subtypes: Carry the leading or trailing byte slice of an MPEG audio frame that crosses a DataStream sector/chunk boundary; reassembled by `MPEGAudioSubscriberThread` before feeding `MPAudioDecode` (`folio 7:0`).

## Disc Inventory (`driving/data/movies/`)

The 10 `.mov` files on disc fall into two structural variants:

| Variant | Files | Layout after `SHDR` (`+0xF4`) |
| --- | --- | --- |
| **Audio + Video (8 files)** | `Intro.mov`, `win1.mov`, `win2.mov`, `lose.mov`, `dnf.mov`, `DriveThru.Testona.mov`, `DriveThru.Laguna.mov`, `DriveThru.NewOrleans.mov` | Starts at `+0xF4` with `'MPAU'` (`'MHDR'`, size `0x1C`) + `'MPAU'` audio data (`0x287`...) + `'MPVD'` (`'VHDR'`, size `0x20`). First MPEG-1 video sequence header (`00 00 01 B3`) begins at file offset `+0x3D0`. |
| **Video-Only (2 files)** | `credits.mov`, `dummy.mov` | Starts at `+0xF4` directly with `'MPVD'` (`'VHDR'`, size `0x20`) + `'MPVD'` video payload (`0x30F8`...). First MPEG-1 sequence header (`00 00 01 B3`) begins at file offset `+0x12C`. Both files are byte-identical (`43,008` bytes) and decode to a 270×200 red `PLACEHOLDER` test card. |
