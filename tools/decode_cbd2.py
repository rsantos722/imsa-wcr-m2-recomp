#!/usr/bin/env python3
"""Decode Panasonic M2 'CBD2' AIFF-C audio ("2:1 Cuberoot-Delta-Exact").

Algorithm confirmed against paired .aiff/.cbd2 voiceovers on disc and
identical to FFmpeg's cbd2_dpcm (libavcodec/dpcm.c):

  per channel predictor p (init 0); for each signed byte n:
      if n is even: p = 0
      p += int(n*n*n / 64)        # C trunc-toward-zero division
      emit p

Container: IFF FORM.AIFC, FVER/COMM/SSND chunks; COMM compressionType
"CBD2", name "CBD2$2:1 Cuberoot-Delta-Exact compression".

outputs ending in .ogg are encoded to OGG Vorbis (q5 VBR, no
normalization) for distribution; WAV masters remain available via
M2_KEEP_WAV=1 or by requesting a .wav output directly.
"""
import argparse
import os
import struct
import subprocess
import sys

# OGG Vorbis distribution encode. Vorbis q≈5 (~160 kbps) keeps the
# CBD2 envelope/dynamics untouched — we never normalize or re-gain.
# Primary path is libsndfile via `soundfile` (no temp WAV, no subprocess);
# ffmpeg with a vorbis-capable build is the fallback.


def write_wav_master(pcm_bytes, wav_path, samplerate=44100, channels=2):
    import wave
    with wave.open(wav_path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(samplerate)
        w.writeframes(pcm_bytes)


def encode_ogg(pcm_bytes, ogg_path, samplerate=44100, channels=2):
    """Encode interleaved S16 PCM to OGG Vorbis. Returns True on success."""
    try:
        import numpy as np
        import soundfile as sf
    except ImportError:
        # ffmpeg fallback (only works on builds shipping a vorbis encoder)
        tmp = os.path.splitext(ogg_path)[0] + ".master.wav"
        write_wav_master(pcm_bytes, tmp, samplerate, channels)
        try:
            subprocess.run(
                ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                 "-i", tmp, "-c:a", "libvorbis", "-qscale:a", "5", ogg_path],
                check=True)
            return True
        except (subprocess.CalledProcessError, FileNotFoundError):
            return False
        finally:
            os.remove(tmp)
    data = np.frombuffer(pcm_bytes, dtype=np.int16)
    data = data[: len(data) // channels * channels].reshape(-1, channels)
    sf.write(ogg_path, data, samplerate, format="OGG", subtype="VORBIS")
    return True


def read_chunks(data):
    if len(data) < 12 or data[:4] != b"FORM":
        raise ValueError("not an IFF file")
    form_size = struct.unpack(">I", data[4:8])[0]
    if form_size < 4:
        raise ValueError("short FORM body")
    form_end = 8 + form_size
    if form_end != len(data):
        raise ValueError(
            f"FORM size mismatch: declared end {form_end}, file {len(data)}"
        )
    form_type = data[8:12]
    off = 12
    out = []
    while off < form_end:
        if off + 8 > form_end:
            raise ValueError(f"truncated IFF chunk header at 0x{off:x}")
        cid = data[off : off + 4]
        sz = struct.unpack(">I", data[off + 4 : off + 8])[0]
        end = off + 8 + sz
        if end > form_end:
            raise ValueError(f"IFF chunk {cid!r} extends past FORM")
        out.append((cid.decode("latin1"), off + 8, sz))
        off = end + (sz & 1)
        if off > form_end:
            raise ValueError(f"IFF chunk {cid!r} padding extends past FORM")
    return form_type.decode("latin1"), out


def cdiv(a, b):
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


TABLE = [cdiv(i * i * i, 64) for i in range(-128, 128)]


def decode_cbd2(raw: bytes, channels: int):
    preds = [0] * channels
    frames = []
    k = 0
    total = len(raw) // channels * channels
    while k < total:
        frame = []
        reset_all = True
        vals = []
        for ch in range(channels):
            n = raw[k + ch]
            ns = n - 256 if n >= 128 else n
            if ns % 2 == 0:
                preds[ch] = 0
            preds[ch] += TABLE[ns + 128]
            vals.append(max(-32768, min(32767, preds[ch])))
        frames.append(vals)
        k += channels
    return frames


def decode_cbd2_fast(raw: bytes, channels: int):
    """Vectorized decode_cbd2 (numpy). Same math, ~100x faster — the predictor
    is a running sum of TABLE[n] that restarts at every even byte, so it is a
    segmented cumulative sum. Returns interleaved int16 PCM."""
    import numpy as np

    usable = len(raw) // channels * channels
    b = np.frombuffer(raw[:usable], dtype=np.uint8).astype(np.int64)
    ns = b.astype(np.int16)
    ns[ns >= 128] -= 256                      # signed bytes (per-channel order)
    steps = np.array(TABLE, dtype=np.int64)[ns + 128]

    if channels == 1:
        even = (ns & 1) == 0
        csum = np.cumsum(steps)
        idx = np.arange(usable, dtype=np.int64)
        last_reset = np.where(even, idx, -1)
        np.maximum.accumulate(last_reset, out=last_reset)
        r = np.maximum(last_reset, 0)
        # sum over j in [r..i] = csum[i] - csum[r-1]; csum[r-1] = csum[r]-step[r]
        prev = np.where(last_reset >= 0, csum[r] - steps[r], 0)
        pcm = csum - prev
    else:
        nsmat = ns.reshape(-1, channels)
        mat = steps.reshape(-1, channels)     # frame-major
        # Reset flag comes from the *byte* parity (e.g. n=-10 is even but its
        # step TABLE[-10]=-15 is odd), not from the step values.
        evenm = ((nsmat & 1) == 0)
        pcm = np.empty_like(mat)
        for ch in range(channels):
            col = mat[:, ch]
            cs = np.cumsum(col)
            idx = np.arange(col.size, dtype=np.int64)
            lr = np.where(evenm[:, ch], idx, -1)
            np.maximum.accumulate(lr, out=lr)
            r = np.maximum(lr, 0)
            prev = np.where(lr >= 0, cs[r] - col[r], 0)
            pcm[:, ch] = cs - prev
        pcm = pcm.reshape(-1)

    return np.clip(pcm, -32768, 32767).astype(np.int16)


def iter_decode_cbd2(raw: bytes, channels: int, block_frames: int = 1 << 20):
    """Yield interleaved int16-LE PCM blocks (streaming `decode_cbd2_fast`).

    The whole-file fast path holds several full-size int64 arrays and the
    single giant vorbis write segfaults on the 29-39 MB soundtrack tracks,
    so large inputs are processed in bounded blocks. The per-channel
    predictor carries across blocks exactly: samples before a block's first
    even-byte reset get the incoming predictor added; at/after a reset the
    block-local segmented sum is already exact. Output is bit-identical to
    `decode_cbd2_fast`.
    """
    import numpy as np

    table = np.array(TABLE, dtype=np.int64)
    total = len(raw) // channels * channels
    carry = [0] * channels
    done = 0
    while done < total:
        blk = raw[done:done + block_frames * channels]
        usable = len(blk) // channels * channels
        blk = blk[:usable]
        b = np.frombuffer(blk, dtype=np.uint8).astype(np.int64)
        ns = b.astype(np.int16)
        ns[ns >= 128] -= 256
        steps = table[ns + 128]
        out = np.empty(usable, dtype=np.int64)
        for ch in range(channels):
            col = steps[ch::channels]
            cs = np.cumsum(col)
            ev = (ns[ch::channels] & 1) == 0
            idx = np.arange(col.size, dtype=np.int64)
            lr = np.where(ev, idx, -1)
            np.maximum.accumulate(lr, out=lr)
            r = np.maximum(lr, 0)
            prev = np.where(lr >= 0, cs[r] - col[r], 0)
            rel = cs - prev
            if ev.any():
                first = int(np.argmax(ev))
                if first > 0:
                    rel[:first] = rel[:first] + carry[ch]
            else:
                rel = rel + carry[ch]
            carry[ch] = int(rel[-1]) if rel.size else carry[ch]
            out[ch::channels] = np.clip(rel, -32768, 32767)
        done += usable
        yield out.astype(np.int16).tobytes()


def aiff_header(channels, frames, rate):
    def chunk(cid, payload):
        return cid + struct.pack(">I", len(payload)) + payload

    ssnd_data = b"\x00" * 8
    comm = struct.pack(">hLh", channels, frames, 16)
    comm += bytes.fromhex("400eac440000000000000000")  # 80-bit float 44100
    parts = b"FORM\x00\x00\x00\x00AIFC"
    parts += chunk(b"FVER", struct.pack(">I", 0xA2805140))
    parts += chunk(b"COMM", comm + b"CBD2\x00" + b"not compressed\x00")
    return parts  # caller appends SSND itself


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input", help=".cbd2 input")
    ap.add_argument("-o", "--output", default=None,
                    help="output .wav or .ogg (extension decides)")
    args = ap.parse_args()

    data = open(args.input, "rb").read()
    ftype, chunks = read_chunks(data)
    assert ftype == "AIFC", f"unexpected FORM type {ftype}"
    comm = next(c for c in chunks if c[0] == "COMM")
    off, sz = comm[1], comm[2]
    nch, frames, bits = struct.unpack(">hLh", data[off : off + 8])
    ssnd = next(c for c in chunks if c[0] == "SSND")
    soff, ssz = ssnd[1], ssnd[2]
    raw = data[soff + 8 : soff + ssz]  # skip offset/blockSize fields

    print(f"{nch}ch {frames} frames @44100 -> decoding {len(raw)} bytes")

    import wave

    out_path = args.output or os.path.splitext(args.input)[0] + ".decoded.wav"

    try:
        import numpy  # noqa: F401
        import numpy as np

        have_numpy = True
    except ImportError:
        have_numpy = False

    if out_path.lower().endswith(".ogg"):
        # OGG distribution copy straight from decoded PCM.
        # Written incrementally so large soundtrack tracks stay bounded.
        if not have_numpy:
            frames_pcm = decode_cbd2(raw, nch)
            interleave = bytearray()
            # de-interleave: our decode produced per-frame channel lists
            for fr in frames_pcm:
                for v in fr:
                    interleave += struct.pack("<h", v)
            pcm_bytes = bytes(interleave)
            if not encode_ogg(pcm_bytes, out_path, 44100, nch):
                raise SystemExit(f"no vorbis encoder available (need soundfile or "
                                 f"vorbis-enabled ffmpeg) for {out_path}")
        else:
            try:
                import soundfile as sf
            except ImportError:
                raise SystemExit(f"no vorbis encoder available (need soundfile) "
                                 f"for {out_path}")
            os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
            keep_wav = os.environ.get("M2_KEEP_WAV") == "1"
            with sf.SoundFile(out_path, mode="w", samplerate=44100,
                              channels=nch, format="OGG",
                              subtype="VORBIS") as f:
                if keep_wav:
                    import wave
                    master = os.path.splitext(out_path)[0] + ".master.wav"
                    wm = wave.open(master, "wb")
                    wm.setnchannels(nch)
                    wm.setsampwidth(2)
                    wm.setframerate(44100)
                for blk in iter_decode_cbd2(raw, nch):
                    f.write(np.frombuffer(blk, dtype=np.int16).reshape(-1, nch))
                    if keep_wav:
                        wm.writeframes(blk)
                if keep_wav:
                    wm.close()
                    print(f"wrote {out_path} (+ WAV master {master})")
                    return 0
        if os.environ.get("M2_KEEP_WAV") == "1" and not have_numpy:
            master = os.path.splitext(out_path)[0] + ".master.wav"
            write_wav_master(pcm_bytes, master, 44100, nch)
            print(f"wrote {out_path} (+ WAV master {master})")
        else:
            print(f"wrote {out_path}")
        return 0

    with wave.open(out_path, "wb") as w:
        w.setnchannels(nch)
        w.setsampwidth(2)
        w.setframerate(44100)
        if have_numpy:
            for blk in iter_decode_cbd2(raw, nch):
                w.writeframes(blk)
        else:
            frames_pcm = decode_cbd2(raw, nch)
            interleave = bytearray()
            for fr in frames_pcm:
                for v in fr:
                    interleave += struct.pack("<h", v)
            w.writeframes(bytes(interleave))
    print(f"wrote {out_path}")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, struct.error, EOFError) as exc:
        sys.exit(f"decode_cbd2: {exc}")
