#!/usr/bin/env python3
"""Decode IMSA ``.imsasound`` files.

The on-disc ``aiff2imsasound`` converter writes a fixed 0x60-byte TagArg
header followed immediately by the sample bytes. ``AF_TAG_NUMBYTES`` is the
valid sample length; any remaining bytes are file padding and are not audio.

The recovered files contain either:

* uncompressed big-endian signed PCM (``AF_TAG_COMPRESSIONTYPE == 0``), or
* M2 hardware SQS2 data (``'SQS2'``), one compressed byte per original
  16-bit mono sample.

The SQS2 byte transform follows the M2 format description and the public 3DO
SDX2 decoder: a signed byte contributes ``sign(x) * 2 * x * x``; an even byte
resets the running predictor before that contribution. The transform is
recovered at the codec level, but these SQS2 assets have no paired original
AIFF on the disc for waveform-level verification.

Usage:
  python tools/decode_imsasound.py \
      "$IMSA_DATA_DIR"/driving/data/aiff/backfire.imsasound --dump
  python tools/decode_imsasound.py \
      "$IMSA_DATA_DIR"/driving/data/aiff/backfire.imsasound -o /tmp/backfire.wav
  python tools/decode_imsasound.py --all --outdir converted/sfx

``--all`` and any ``-o`` path ending in ``.ogg`` produce OGG Vorbis copies
(q5 VBR, no normalization) for host streaming via ``host/audio/audio_ogg.cpp``.
"""
import argparse
import glob
import math
import os
import struct


TAG_END = 0x00
TAG_SUSTAIN_BEGIN = 0x1F
TAG_SUSTAIN_END = 0x20
TAG_RELEASE_BEGIN = 0x21
TAG_RELEASE_END = 0x22
TAG_NUMBYTES = 0x23
TAG_ADDRESS = 0x24
TAG_CHANNELS = 0x17
TAG_COMPRESSION_RATIO = 0x2F
TAG_COMPRESSION_TYPE = 0x30
TAG_NUMBITS = 0x31
TAG_SAMPLE_RATE_FP = 0x4F
COMPRESSION_SQS2 = int.from_bytes(b"SQS2", "big")

# The converter's sampletags object is 12 TagArgs, including TAG_END.
HEADER_TAG_COUNT = 12
HEADER_SIZE = HEADER_TAG_COUNT * 8

TAG_NAMES = {
    TAG_SUSTAIN_BEGIN: "sustain_begin",
    TAG_SUSTAIN_END: "sustain_end",
    TAG_RELEASE_BEGIN: "release_begin",
    TAG_RELEASE_END: "release_end",
    TAG_NUMBYTES: "num_bytes",
    TAG_ADDRESS: "address",
    TAG_CHANNELS: "channels",
    TAG_COMPRESSION_RATIO: "compression_ratio",
    TAG_COMPRESSION_TYPE: "compression_type",
    TAG_NUMBITS: "num_bits",
    TAG_SAMPLE_RATE_FP: "sample_rate_fp",
    # Three first-generation voiceover files carry this unrecognized legacy
    # query tag in place of 0x4f. Preserve it instead of assigning semantics.
    0x50: "legacy_tag_0x50",
    TAG_END: "TAG_END",
}


def parse_tags(data):
    """Return the fixed header tags and the exact payload offset.

    The converter's ``sampletags`` array and every recovered file pin the
    header at 12 entries / 0x60 bytes, so parsing is deliberately bounded.
    The return shape is ``(tags, payload_offset)``.
    """
    if len(data) < HEADER_SIZE:
        raise ValueError(
            f"short imsasound header: {len(data)} bytes, need {HEADER_SIZE}"
        )

    tags = [
        struct.unpack_from(">II", data, index * 8)
        for index in range(HEADER_TAG_COUNT)
    ]
    if tags[-1][0] != TAG_END:
        raise ValueError(
            f"imsasound header has no TAG_END at 0x{HEADER_SIZE - 8:x}"
        )
    if any(tag == TAG_END for tag, _ in tags[:-1]):
        raise ValueError("imsasound header contains an early TAG_END")
    return tags, HEADER_SIZE


def _fields(tags):
    return dict(tags[:-1])


def _read_file(path):
    data = open(path, "rb").read()
    tags, payload_offset = parse_tags(data)
    fields = _fields(tags)
    if TAG_NUMBYTES not in fields:
        raise ValueError("imsasound header has no AF_TAG_NUMBYTES")

    data_size = fields[TAG_NUMBYTES]
    payload_end = payload_offset + data_size
    if payload_end > len(data):
        raise ValueError(
            f"sample extends past EOF: end 0x{payload_end:x}, file {len(data)}"
        )
    return data, tags, fields, payload_offset, data[payload_offset:payload_end]


def _signed32(value):
    return struct.unpack(">i", struct.pack(">I", value))[0]


def _fourcc(value):
    return value.to_bytes(4, "big").decode("latin1")


def _sample_rate(fields, default):
    value = fields.get(TAG_SAMPLE_RATE_FP)
    if value is None:
        return default
    rate = struct.unpack(">f", struct.pack(">I", value))[0]
    if not math.isfinite(rate) or rate <= 0:
        raise ValueError(f"invalid sample rate {rate!r}")
    return int(rate)


def decode_sqs2(raw, channels):
    """Decode the M2 SQS2 square-root exact/delta byte stream.

    SQS2 is a hardware 2:1 format. The stream is interleaved by channel;
    each channel has an independent predictor. A clear low bit selects an
    exact value, while an odd byte contributes a quarter-scale delta. The
    transform matches DecodeSQXD() in the original M2 DSP simulator.
    """
    if channels not in (1, 2):
        raise ValueError(f"SQS2 supports mono/stereo only, got {channels}")

    predictors = [0] * channels
    samples = []
    usable = len(raw) - (len(raw) % channels)
    for index in range(usable):
        channel = index % channels
        coded = raw[index]
        magnitude = coded & 0xFE
        if magnitude >= 128:
            magnitude -= 256
        expanded = magnitude * abs(magnitude) * 2
        if coded & 1:
            output = predictors[channel] + (expanded >> 2)
        else:
            output = expanded
        predictors[channel] = output & 0xFFFF
        samples.append(
            predictors[channel]
            if predictors[channel] < 0x8000
            else predictors[channel] - 0x10000
        )
    return samples


def _pack_s16le(samples):
    return struct.pack("<" + "h" * len(samples), *samples)


def dump_tags(path):
    data, tags, fields, payload_offset, payload = _read_file(path)
    trailing = len(data) - payload_offset - len(payload)
    rate = _sample_rate(fields, None)
    rate_text = f"{rate} Hz" if rate is not None else "unknown"
    compression = fields.get(TAG_COMPRESSION_TYPE, 0)
    compression_text = (
        f"{_fourcc(compression)!r} (0x{compression:08x})"
        if compression
        else "none (0x00000000)"
    )
    print(
        f"{path}: {len(data)} bytes, header=0x{payload_offset:x}, "
        f"payload={len(payload)} bytes, trailing_padding={trailing} bytes"
    )
    print(f"  decoded format: {compression_text}, rate={rate_text}")
    for tag, arg in tags:
        name = TAG_NAMES.get(tag, f"tag_{tag:#x}")
        if tag == TAG_SAMPLE_RATE_FP:
            value = f"{struct.unpack('>f', struct.pack('>I', arg))[0]:g} Hz"
        elif tag == TAG_COMPRESSION_TYPE:
            value = f"{_fourcc(arg)!r} (0x{arg:08x})" if arg else "none"
        elif tag == TAG_ADDRESS:
            value = f"0x{arg:08x} (serialized item address)"
        elif tag in {
            TAG_SUSTAIN_BEGIN,
            TAG_SUSTAIN_END,
            TAG_RELEASE_BEGIN,
            TAG_RELEASE_END,
        }:
            value = str(_signed32(arg))
        else:
            value = f"{arg} (0x{arg:08x})"
        print(f"  {tag:08x} {name:22s} = {value}")
    if TAG_SAMPLE_RATE_FP not in fields:
        print("  note: no current AF_TAG_SAMPLE_RATE_FP; caller default may be used")
    print(f"  payload first 32: {payload[:32].hex()}")


def extract_payload(path):
    """Return header tags and only the ``AF_TAG_NUMBYTES`` sample bytes."""
    _, tags, _, _, payload = _read_file(path)
    return tags, payload


def build_pcm(path, samplerate=22050, channels=1, bits=16):
    """Decode one file into interleaved little-endian PCM for WAV/OGG."""
    _, _, fields, _, payload = _read_file(path)
    samplerate = _sample_rate(fields, samplerate)
    channels = fields.get(TAG_CHANNELS, channels)
    bits = fields.get(TAG_NUMBITS, bits)
    compression = fields.get(TAG_COMPRESSION_TYPE, 0)

    if channels not in (1, 2):
        raise ValueError(f"unsupported channel count {channels}")
    if bits not in (8, 16):
        raise ValueError(f"unsupported sample width {bits} bits")

    if compression == COMPRESSION_SQS2:
        if bits != 16 or fields.get(TAG_COMPRESSION_RATIO, 2) != 2:
            raise ValueError("unsupported SQS2 parameters")
        pcm = _pack_s16le(decode_sqs2(payload, channels))
    elif compression == 0:
        frame_bytes = channels * (bits // 8)
        if len(payload) % frame_bytes:
            raise ValueError(
                f"uncompressed payload is not frame-aligned: "
                f"{len(payload)} bytes for {frame_bytes}-byte frames"
            )
        if bits == 16:
            samples = struct.unpack(">" + "h" * (len(payload) // 2), payload)
            pcm = _pack_s16le(samples)
        else:
            pcm = payload
    else:
        raise ValueError(f"unsupported compression type 0x{compression:08x}")

    return pcm, samplerate, channels, bits


def write_wav(path, out_path, samplerate=22050, channels=1, bits=16):
    import wave

    pcm, samplerate, channels, bits = build_pcm(
        path, samplerate, channels, bits
    )

    if out_path.lower().endswith(".ogg"):
        from decode_cbd2 import encode_ogg, write_wav_master

        if not encode_ogg(pcm, out_path, samplerate, channels):
            raise SystemExit(f"no vorbis encoder available for {out_path}")
        if os.environ.get("M2_KEEP_WAV") == "1":
            write_wav_master(
                pcm,
                os.path.splitext(out_path)[0] + ".master.wav",
                samplerate,
                channels,
            )
        return

    with wave.open(out_path, "wb") as wave_file:
        wave_file.setnchannels(channels)
        wave_file.setsampwidth(2 if bits == 16 else 1)
        wave_file.setframerate(samplerate)
        wave_file.writeframes(pcm)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input", nargs="?", help=".imsasound file")
    ap.add_argument("-o", "--output", help="output .wav or .ogg")
    ap.add_argument("--dump", action="store_true", help="dump header tags")
    ap.add_argument("--all", action="store_true", help="process all .imsasound")
    ap.add_argument("--outdir", default="converted/sfx")
    ap.add_argument("--root", default=os.environ.get("IMSA_DATA_DIR"),
                    help="extracted game-data root for --all (default: $IMSA_DATA_DIR)")
    args = ap.parse_args()

    if args.all:

        if not args.root:

            ap.error("--all needs --root or IMSA_DATA_DIR")
        files = glob.glob(os.path.join(args.root, "**", "*.imsasound"), recursive=True)
        os.makedirs(args.outdir, exist_ok=True)
        failed = 0
        for path in sorted(files):
            out = os.path.join(args.outdir, os.path.relpath(path, args.root))
            out = os.path.splitext(out)[0] + ".ogg"
            os.makedirs(os.path.dirname(out), exist_ok=True)
            try:
                write_wav(path, out)
                print(f"wrote {out}")
            except Exception as error:
                failed += 1
                print(f"FAIL {path}: {error}")
        return 1 if failed else 0

    if args.dump:
        if not args.input:
            ap.error("--dump requires an input file")
        dump_tags(args.input)
        return 0

    if not args.input:
        ap.print_help()
        return 1
    out = args.output or os.path.splitext(args.input)[0] + ".wav"
    write_wav(args.input, out)
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
