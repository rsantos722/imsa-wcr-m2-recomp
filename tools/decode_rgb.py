#!/usr/bin/env python3
"""Decode M2 .RGB (RAW RGB) to PNG.

Format (docs/formats/rgb.md):
  +0x00 8 bytes magic "RAW RGB "
  +0x08 u16 BE width
  +0x0A u16 BE height
  +0x0C ... width*height*2 bytes BE RGB555 (x1RRRRRGGGGGBBBBB)

All 4 files are 576x432.
"""
import struct, os, argparse, glob

def decode_rgb(path, out):
    from PIL import Image

    data = open(path, 'rb').read()
    if len(data) < 12 or data[:8] != b'RAW RGB ':
        raise ValueError(f"{path}: not a RAW RGB file")
    w, h = struct.unpack('>HH', data[8:12])
    expected = 12 + w*h*2
    if len(data) < expected:
        raise ValueError(
            f"{path} size {len(data)} < expected {expected} for {w}x{h}")
    pix = data[12:expected]
    img = Image.new('RGBA', (w,h))
    p = img.load()
    idx=0
    for y in range(h):
        for x in range(w):
            v = struct.unpack('>H', pix[idx:idx+2])[0]
            idx+=2
            # x1RRRRRGGGGGBBBBB -> 5 bits each
            r = ((v>>10)&0x1F)*255//31
            g = ((v>>5)&0x1F)*255//31
            b = (v&0x1F)*255//31
            p[x,y]=(r,g,b,255)
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    img.save(out)
    print(f"wrote {out} {w}x{h}")

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("input", nargs="?")
    ap.add_argument("-o","--output")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--outdir", default="converted/stills")
    ap.add_argument("--root", default=os.environ.get("IMSA_DATA_DIR"),
                    help="extracted game-data root for --all (default: $IMSA_DATA_DIR)")
    args=ap.parse_args()
    if args.all:
        if not args.root:
            ap.error("--all needs --root or IMSA_DATA_DIR")
        files=glob.glob(os.path.join(args.root, "**", "*.RGB"), recursive=True)
        for f in files:
            out=os.path.join(args.outdir, os.path.relpath(f,args.root))+".png"
            decode_rgb(f,out)
        return
    if not args.input:
        ap.print_help()
        return 1
    out=args.output or os.path.splitext(args.input)[0]+".png"
    decode_rgb(args.input,out)

if __name__=="__main__":
    raise SystemExit(main())
