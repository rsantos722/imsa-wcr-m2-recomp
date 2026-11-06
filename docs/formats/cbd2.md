# CBD2 (`FORM.AIFC`) Audio Format

Music tracks (`driving/data/Music/*.cbd2`), race voiceovers (`driving/data/Music/voiceovers/*.cbd2`), and system chimes (`driving/data/Music/bong.44s.cbd2`, `shooter2d/*.aifc`) use 3DO's **CBD2** (*2:1 Cuberoot-Delta-Exact*) compressed AIFF-C format.

The standalone decoder and WAV converter is in [`tools/decode_cbd2.py`](../../tools/decode_cbd2.py).

## IFF `FORM.AIFC` Container Structure

Each `.cbd2` file is a big-endian IFF `FORM` container with form type `'AIFC'` containing standard AIFF-C chunks:

| Chunk ID | Description |
|---|---|
| `FVER` | Format version (`0xA2805140`). |
| `COMM` | Common attributes: `u16 numChannels` (`1` mono for voiceovers, `2` stereo for music), `u32 numSampleFrames`, `u16 sampleSize` (`16`), `80-bit IEEE extended sampleRate` (`44100.0` Hz or `22050.0` Hz), `char[4] compressionType` (`'CBD2'`), and P-string `"CBD2$2:1 Cuberoot-Delta-Exact compression"`. |
| `MARK` | Optional loop marker definitions. |
| `INST` | Optional instrument sustain/release loop markers. |
| `SSND` | Sound data chunk: `u32 offset` (`0`), `u32 blockSize` (`0`), followed immediately by `numSampleFrames * numChannels` bytes of CBD2-encoded deltas (1 byte per 16-bit output sample). |

> [!NOTE]
> The four voiceover files in `driving/data/Music/voiceovers/` (`Laguna`, `NewOrleans`, `Suzuka`, `Testona`) have both `.cbd2` and uncompressed `.aiff` versions on disc. Those `.aiff` files have an odd top-level `FORM` chunk size plus a trailing `APPL` chunk that Portfolio OS's word-aligned IFF parser (`ParseAIFF` at `0x6f3cc` / `ScanAIFF` at `0x6fc20`) rejects with `IFF_ERR_MANGLED` (`0xd56d5103`), while announcer playback uses the valid `.cbd2` files.

## CBD2 Decompression Algorithm

CBD2 compresses 16-bit signed PCM at a fixed 2:1 ratio (1 signed byte per sample) using a cubic delta lookup table and an exact-reset LSB flag. On M2 hardware, decompression is executed by the Bulldog DSPP using the `System.m2/Audio/dsp/sampler_cbd2_{v1,v2,v4,f1,f2}.dsp` microcode templates.

### Cubic Delta Lookup Table

For every signed 8-bit integer $n \in [-128, 127]$:

$$\text{TABLE}[n] = \text{trunc\_toward\_zero}\!\left(\frac{n^3}{64}\right) = \begin{cases} \lfloor n^3 / 64 \rfloor & n \ge 0 \\ -\lfloor (-n)^3 / 64 \rfloor & n < 0 \end{cases}$$

The maximum positive step is $\lfloor 127^3 / 64 \rfloor = 32,006$, allowing a single byte to traverse nearly the entire signed 16-bit range.

### Per-Channel Predictor State Machine

Each channel $c \in [0, \text{numChannels}-1]$ maintains a 16-bit signed accumulator $p_c$, initialized to `0` at the start of the stream. Samples are frame-interleaved (`L, R, L, R, ...` for stereo).

For each input byte $b$ belonging to channel $c$ (interpreted as signed $n \in [-128, 127]$):

1. **Exact/Reset Check (`(n & 1) == 0`)**:
   - If `(n & 1) == 0` (even): reset $p_c \leftarrow 0$ before adding the cubic step.
   - If `(n & 1) == 1` (odd): keep the running predictor $p_c$ (delta mode).
2. **Accumulate and Clamp**:
   $$p_c \leftarrow \text{clamp}(p_c + \text{TABLE}[n], -32768, 32767)$$
3. Emit $p_c$ as the decoded 16-bit signed PCM sample for channel $c$.

## Disc Inventory (`driving/data/Music/`)

- **Music Tracks (`44.1 kHz` Stereo CBD2)**:
  - `driving/data/Music/ui_music.cbd2`
  - `driving/data/Music/grunge_{fast,med,slow}.cbd2`
  - `driving/data/Music/pop_{fast,med,slow}.cbd2`
  - `driving/data/Music/rock_{fast,med,slow}.cbd2`
  - `driving/data/Music/techno_{fast,med,slow}.cbd2`
  - `driving/data/Music/bong.44s.cbd2`
- **Voiceovers (`22.05 kHz` / `44.1 kHz` Mono CBD2)**:
  - `driving/data/Music/voiceovers/{Laguna,NewOrleans,Suzuka,Testona}.cbd2`

## Usage

```bash
# Decode one track to 16-bit PCM WAV
mkdir -p converted
python3 tools/decode_cbd2.py extracted/driving/data/Music/ui_music.cbd2 -o converted/ui_music.wav
```
