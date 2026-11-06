# MPEG Video Device and Audio Decoder APIs

The 3DO M2 platform includes hardware-accelerated MPEG-1 Video decoding (`mpegvideo.device`, `CMDC_MPEGV = 0x0007`) and a DSP/CPU **MPEG-1 Layer II Audio Decoder Folio** (`System.m2/Modules/mpegaudiodecoder`, `module 23` / host `folio 7`).

*IMSA World Championship Racing* uses these subsystems for:
1. **16 UI Background Still Images (`MPS`)** embedded in `driving/data/ui/uiart.memimage` (`MPS_LoadMPEGStill`, `MPS_InitMPEGStill`, `MPS_DrawMPEGStill`).
2. **Full-Screen FMV Sequences (`576×432` / `320×240`)**: `intro.mov`, `win1.mov`, `win2.mov`, `lose.mov`, `dnf.mov`, and `credits.mov` (`VideoPlayer` at `0x3c27c`).
3. **Configure Race Track Preview Overlays (`204×128` at `(66, 250)`)**: `DriveThru.Testona.mov`, `DriveThru.Laguna.mov`, `DriveThru.NewOrleans.mov`, and `dummy.mov`.

## `mpegvideo.device` (`CMDC_MPEGV = 0x0007`)

Guest code opens the `"mpegvideo"` device stack through a `DeviceStack` list (`CreateDeviceStackList` `1:67`) and `OpenDeviceStack` (`1:69`), then submits `IOInfo` commands via `DoIO` (`1:132`) or `SendIO` (`1:142`).

### Command Reference (`ioi_Command`)

| Command Name | Code | Role & Parameters |
|---|---|---|
| `CMD_READ` / `MPEGVIDEOCMD_READ` | `0x00000002` / `0x00070005` | Dequeues the next decoded video frame, converts/scales YUV420 to the destination `Bitmap` Item in `ioi_CmdOptions` (or `ioi_Recv.iob_Buffer`), sets `io_Actual`, writes the frame's 90 kHz PTS to `ioi_Offset` (or `FMVGetPTS` at `0x65114`), and sets `io_Flags = IO_DONE \| 0x80000000` when a valid PTS is attached. |
| `MPEGVIDEOCMD_WRITE` | `0x00070004` | Submits a compressed MPEG-1 video bitstream chunk (`ioi_Send.iob_Buffer`, `ioi_Send.iob_Len` bytes) along with an input `FMVIOReqOptions` descriptor (`ioi_CmdOptions` pointing to `{ uint32 fmvOpt_Flags, uint32 fmvOpt_PTS, uint32 fmvOpt_UserData }`). |
| `MPEGVIDEOCMD_CONTROL` | `0x00070003` | Applies a `TagArg` list in `ioi_Send.iob_Buffer` (`VID_CODEC_TAG_M2MODE`, `VID_CODEC_TAG_PLAY`, `VID_CODEC_TAG_DITHER`, `VID_CODEC_TAG_DEPTH` `16`/`24`, `VID_CODEC_TAG_STANDARD`, `VID_CODEC_TAG_HSIZE`, `VID_CODEC_TAG_VSIZE`, `VID_CODEC_TAG_SKIPFRAMES`, `VID_CODEC_TAG_KEYFRAMES`). |
| `MPEGVIDEOCMD_GETBUFFERINFO` | `0x00070006` | Populates `MPEGBufferInfo` (`24` bytes) in `ioi_Recv.iob_Buffer` based on the `TagArg` query in `ioi_Send.iob_Buffer` (`CalcBufferInfo`). |
| `MPEGVIDEOCMD_ALLOCBUFFERS` | `0x00070009` | Negotiates strip and reference buffer capacities (`MPEGBufferAllocInfo` in `ioi_Send.iob_Buffer`). |
| `MPEGVIDEOCMD_SETSTRIPBUFFER` | `0x00070007` | Registers a page-aligned strip buffer allocated via `AllocMemMasked` (`16 KB`, `32 KB`, `64 KB`, or `128 KB`). This `IOReq` stays pending while the decoder owns the buffer until aborted via `AbortIO`. |
| `MPEGVIDEOCMD_SETREFERENCEBUFFER` | `0x00070008` | Registers one of two page-aligned YUV reference frame buffers (`width * height * 3 / 2` bytes, `careBits = 0xFFF`). Like `SETSTRIPBUFFER`, stays pending until `AbortIO`. |

### `MPEGBufferInfo` Layout (`MPEGVIDEOCMD_GETBUFFERINFO`, 24 bytes)

```c
struct MPEGBufferInfo {
    uint32_t refBufferSize;     // +0x00: Byte size of 1 YUV420 reference frame (e.g. 576*432*3/2 = 373,248)
    uint32_t refMemCareBits;    // +0x04: Alignment care mask (0x00000FFF for 4 KB page alignment)
    uint32_t refMemStateBits;   // +0x08: Alignment state bits (0x00000000)
    uint32_t stripBufferSize;   // +0x0C: Strip buffer size (16384, 32768, 65536, or 131072)
    uint32_t stripMemCareBits;  // +0x10: Strip alignment care mask (0x00000FFF)
    uint32_t stripMemStateBits; // +0x14: Strip alignment state bits (0x00000000)
};
```

## Still-Picture Device Use

To decode a still background image (`MPS_InitMPEGStill`, `MPS_LoadMPEGStill`, `MPS_DrawMPEGStill`), the guest opens `mpegvideo.device`, queries buffer requirements, registers strip and reference buffers, writes the compressed I-frame twice to flush the hardware's one-frame reorder delay, and reads out the decoded RGB555 bitmap.

For how menu screens use these stills and how the host handles headerless post-race still packets, see [UI States and Video Screens](../game/menus.md#ui-states-and-video-screens), [`.memimage` Container Layout](../formats/memimage.md#memimage-binary-container-layout), and [Folios 5, 6, and 7](../recompilation/shims.md#folios-5-6-and-7-audiopatch-audio-and-mpegaudiodecoder).

## MPEG Audio Decoder Folio (`folio 7`, `mpegaudiodecoder`)

`driving/game` imports all 4 functions from `System.m2/Modules/mpegaudiodecoder` (`module 23`, host `folio 7`):

| Ordinal | Prototype | Behavior |
|---|---|---|
| `7:0` | `Err MPAudioDecode(MPADecoderContext *ctx, uint32 *pts, uint32 *ptsValid, uint32 *decompressedBufPtr)` | Invokes the registered `GetCompressedBfrFn` (`ctx->getCompressedBfr`) and `CompressedBfrReadFn` (`ctx->compressedBfrRead`) callbacks to pull compressed MPEG-1 Layer II bytes, decodes 1 MPEG audio frame (`1,152` stereo 16-bit big-endian PCM samples = `4,608` bytes at `44.1 kHz`) into the buffer provided to `CreateMPAudioDecoder`, and updates `*pts` and `*ptsValid`. |
| `7:1` | `Err CreateMPAudioDecoder(MPADecoderContext **ctxOut, const TagArg *tags)` | Allocates decoder state (`MPA_TAG_GET_COMPRESSED_BFR_FN = 1`, `MPA_TAG_COMPRESSED_BFR_READ_FN = 2`, `MPA_TAG_CLOSE_DECODER_FN = 3`, `MPA_TAG_OUTPUT_BUFFER = 4`). |
| `7:2` | `Err DeleteMPAudioDecoder(MPADecoderContext *ctx)` | Frees the decoder instance. |
| `7:3` | `Err MPAFlush(MPADecoderContext *ctx)` | Resets internal bitstream reservoir and synthesis filterbank state on stream seek/stop. |

### Callback and PTS Propagation Rules

- `GetCompressedBfrFn(void *userCtx, const uint8 **bufOut, uint32 *lenOut, uint32 *ptsOut, uint32 *ptsValidOut, uint32 *wholeFrameOut)` is implemented by `GetCompressedBfr` (`0x63be8`), and byte consumption is reported via `CompressedBfrRead` (`0x63c9c`).
- When an `'MPAU'` chunk contains multiple frames (`'2FRM'..'4FRM'`) or split frames across chunk boundaries, `*ptsValid` is only asserted on the first whole frame beginning inside a chunk that carried a valid PTS (`PTSIsValid && wholeFrame`).
- Decoded PCM is written into the client-provided output buffer, which the game queues through `SoundSpooler` to advance its audio presentation clock (see [DataStream Video Playback](../game/audio.md#datastream-video-playback)).
