# `.imsasound` Sound Bank and SQS2 Audio Format

Sound effects, including engines, tires, impacts, and crew chief audio (47 files under `driving/data/aiff/` and `driving/data/tracks/*/`) are stored in a custom `.imsasound` format.

The format was produced offline by the on-disc PowerPC tool `driving/data/aiff/aiff2imsasound` (`SampleInfoToTags` at `0x00000A10`) and is decoded by [`tools/decode_imsasound.py`](../../tools/decode_imsasound.py).

## Binary Layout (`0x60`-Byte `TagArg` Header + Payload)

An `.imsasound` file is a serialized Portfolio Audio Folio `TagArg` array followed immediately at offset `0x60` (96 bytes) by the raw sample payload, padded at the end to a 2048-byte sector boundary.

The 96-byte header consists of 12 big-endian `TagArg` pairs (`struct { uint32_t ta_Tag; uint32_t ta_Arg; }`, 8 bytes each):

| Entry | Offset | Tag ID | Portfolio Symbol | Value / Description |
|---|---|---|---|---|
| `0` | `+0x00` | `0x00000024` | `AF_TAG_ADDRESS` | Serialized buffer pointer (ignored on disk; patched in-place at `buffer + 0x04` to `buffer + 0x60` by `imsasound_buffertosample` at `0x2c7c0`). |
| `1` | `+0x08` | `0x0000004F` (or `0x50`) | `AF_TAG_SAMPLE_RATE_FP` | IEEE-754 32-bit float sample rate (`44100.0f` = `0x472C4400`, `22050.0f` = `0x46AC4400`, `11025.0f` = `0x462C4400`). Note: 3 early voiceover files emit tag `0x50` with integer `0`, which defaults to `22050` Hz. |
| `2` | `+0x10` | `0x00000017` | `AF_TAG_CHANNELS` | Channel count (`1` = mono, `2` = stereo). |
| `3` | `+0x18` | `0x0000002F` | `AF_TAG_COMPRESSIONRATIO` | Compression ratio: `1` for uncompressed 16-bit PCM, `2` for `SQS2`. |
| `4` | `+0x20` | `0x00000030` | `AF_TAG_COMPRESSIONTYPE` | `0x00000000` (uncompressed big-endian 16-bit signed PCM) or `'SQS2'` (`0x53515332`). |
| `5` | `+0x28` | `0x00000031` | `AF_TAG_NUMBITS` | Uncompressed bit depth (`16`). |
| `6` | `+0x30` | `0x00000023` | `AF_TAG_NUMBYTES` | Exact byte length of the audio payload starting at `+0x60`. |
| `7` | `+0x38` | `0x0000001F` | `AF_TAG_SUSTAINBEGIN` | Sustain loop start frame index (`0xFFFFFFFF` / `-1` if no loop; present in 24 of 47 files). |
| `8` | `+0x40` | `0x00000020` | `AF_TAG_SUSTAINEND` | Sustain loop end frame index (`0xFFFFFFFF` / `-1` if no loop). |
| `9` | `+0x48` | `0x00000021` | `AF_TAG_RELEASEBEGIN` | Release loop start frame index (`0xFFFFFFFF` / `-1` in all 47 files). |
| `10` | `+0x50` | `0x00000022` | `AF_TAG_RELEASEEND` | Release loop end frame index (`0xFFFFFFFF` / `-1` in all 47 files). |
| `11` | `+0x58` | `0x00000000` | `TAG_END` | `{ 0x00000000, 0x00000000 }` terminator. |

## SQS2 (`0x53515332`) Decompression Algorithm

When `AF_TAG_COMPRESSIONTYPE == 'SQS2'` (`0x53515332`), the payload at `+0x60` is encoded with 3DO's **2:1 Squareroot-Delta-Exact** compression (`DRSC_INFIFO_SUBTYPE_SQS2`, decoded on hardware by `System.m2/Audio/dsp/sampler_sqs2_v1.dsp` and `sampler_sqs2_f1.dsp`).

Each channel $c$ maintains a 16-bit signed accumulator $p_c$, initialized to `0`. For each signed input byte $x \in [-128, 127]$ belonging to channel $c$:

1. **Exact/Reset Check (LSB == 0)**:
   - If $(x \ \& \ 1) == 0$ (even byte value), reset the predictor before adding the step:
     $$p_c \leftarrow 0$$
2. **Signed Quadratic Step (`2 * x * |x|`)**:
   $$\Delta = \begin{cases} 2 x^2 & x \ge 0 \\ -2 x^2 & x < 0 \end{cases}$$
3. **Accumulate and Clamp**:
   $$p_c \leftarrow \text{clamp}(p_c + \Delta, -32768, 32767)$$
4. Emit $p_c$ as the decoded 16-bit signed PCM sample.

When `AF_TAG_COMPRESSIONTYPE == 0`, the payload at `+0x60` is raw big-endian 16-bit signed PCM (`>h`).

## Usage

```bash
# Inspect header tags and decode an .imsasound file to WAV
mkdir -p converted
python3 tools/decode_imsasound.py extracted/driving/data/aiff/ferrari.imsasound -o converted/engine.wav
```
