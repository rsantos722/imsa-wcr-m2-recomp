#!/usr/bin/env python3
"""Extract symbol and source-file maps from a driving/game ELF.

The input is an ET_REL, e_machine 0x11 (M2 PPC602 variant) image. Section
headers are parsed directly (big-endian), including .symtab/.strtab/.debug.

Usage:
  python tools/extract_symbols.py --elf "$IMSA_DATA_DIR/driving/game" --outdir converted/symbols
"""
import argparse
import os, struct, re, json, collections

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = None
OUT_MD = os.path.join(REPO, "converted", "symbols", "function-map.md")
OUT_JSON = os.path.join(REPO, "converted", "symbols", "function-map.json")

def parse_elf(path):
    d = open(path, 'rb').read()
    e_shoff = struct.unpack('>I', d[0x20:0x24])[0]
    e_shentsize = struct.unpack('>H', d[0x2E:0x30])[0]
    e_shnum = struct.unpack('>H', d[0x30:0x32])[0]
    e_shstrndx = struct.unpack('>H', d[0x32:0x34])[0]
    sections=[]
    for i in range(e_shnum):
        off=e_shoff+i*e_shentsize
        sh=struct.unpack('>IIIIIIIIII', d[off:off+40])
        sections.append(sh)
    shstr=sections[e_shstrndx]
    shstr_data=d[shstr[4]:shstr[4]+shstr[5]]
    names=[]
    for i,sh in enumerate(sections):
        name_off=sh[0]
        name_end=shstr_data.find(b'\x00', name_off)
        name=shstr_data[name_off:name_end].decode(errors='ignore') if name_end!=-1 else ''
        names.append(name)
    # find symtab
    symtab=None
    strtab=None
    debug=None
    for i,sh in enumerate(sections):
        if names[i]=='.symtab':
            symtab=sh
            strtab=sections[sh[6]]
        if names[i]=='.debug':
            debug=sh
    return d, sections, names, symtab, strtab, debug

def main(argv=None):
    global ELF, OUT_MD, OUT_JSON
    ap = argparse.ArgumentParser(description="Extract function and source-file maps from a guest ELF.")
    data_dir = os.environ.get("IMSA_DATA_DIR")
    default_elf = os.path.join(data_dir, "driving", "game") if data_dir else None
    ap.add_argument("--elf", default=default_elf,
                    help="path to the non-stripped guest ELF (or set IMSA_DATA_DIR)")
    ap.add_argument("--outdir", default=os.path.join(REPO, "converted", "symbols"),
                    help="directory for function-map.md and function-map.json")
    args = ap.parse_args(argv)
    if not args.elf:
        ap.error("--elf is required unless IMSA_DATA_DIR is set")
    if not os.path.exists(args.elf):
        ap.error(f"guest ELF not found: {args.elf}")
    ELF = args.elf
    OUT_MD = os.path.join(args.outdir, "function-map.md")
    OUT_JSON = os.path.join(args.outdir, "function-map.json")
    d, sections, names, symtab, strtab, debug = parse_elf(ELF)
    print(f"sections: {names}")
    print(f"symtab {symtab} strtab {strtab} debug {debug}")
    str_data = d[strtab[4]:strtab[4]+strtab[5]]
    sym_data = d[symtab[4]:symtab[4]+symtab[5]]
    n = len(sym_data)//16
    print(f"symbols {n}")
    # collect function symbols (STT_FUNC =2) and object?
    funcs=[]
    files=set()
    for j in range(n):
        off=j*16
        st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack('>III BBH', sym_data[off:off+16])
        typ = st_info & 0xF
        bind = st_info>>4
        name_end=str_data.find(b'\x00', st_name)
        sname=str_data[st_name:name_end].decode(errors='ignore') if st_name and name_end!=-1 else ''
        # .debug contains source file names
        if sname.endswith('.c') or sname.endswith('.cpp'):
            files.add(sname)
        if typ==2:  # STT_FUNC
            funcs.append((sname, st_value, st_size, st_shndx, bind))
        elif typ==1 and st_size>0:
            # object, maybe useful
            pass
    # Extract source files from .debug strings
    dbg_data = d[debug[4]:debug[4]+debug[5]] if debug else b''
    # .debug is Diab debug format, but contains file names as strings
    dbg_strs = re.findall(rb'[A-Za-z0-9_]+\.c', dbg_data)
    dbg_files = sorted(set(s.decode() for s in dbg_strs))
    print(f"files from symtab {sorted(files)[:10]}...")
    print(f"files from debug {dbg_files[:20]}...")
    all_files = sorted(set(list(files) + dbg_files))
    # Write markdown
    os.makedirs(os.path.dirname(OUT_MD), exist_ok=True)
    with open(OUT_MD, 'w', encoding='utf-8') as out:
        out.write("# Function map — `driving/game` (PPC602, Diab 3.7a)\n\n")
        out.write(f"Source: `{ELF}` — {os.path.getsize(ELF)} bytes, non-stripped ET_REL, e_machine 0x11\n\n")
        out.write(f"Total symbols: {n}, STT_FUNC: {len(funcs)}, source files: {len(all_files)}\n\n")
        out.write("## Source files (from symtab + .debug)\n\n")
        for f in all_files:
            out.write(f"- `{f}`\n")
        out.write("\n## Functions (address = section offset, size = bytes)\n\n")
        out.write("| Address | Size | Shndx | Bind | Name |\n")
        out.write("|---------|------|-------|------|------|\n")
        for name, val, sz, shndx, bind in sorted(funcs, key=lambda x: x[1]):
            b = "GLOBAL" if bind==1 else "LOCAL" if bind==0 else str(bind)
            out.write(f"| {val:#08x} | {sz} | {shndx} | {b} | `{name}` |\n")
        out.write("\n## Notes\n\n")
        out.write("- Addresses are section-relative (ET_REL, not linked). Ghidra import should treat as relocatable.\n")
        out.write("- Compiler: Diab Data `dcc` Rel 3.7a PPC602 (stamp in .debug, also `XDiab Data, Inc:dcc Rel 3.7a:PPC602`).\n")
        out.write("- For a Ghidra PPC:BE:32 import, override e_machine 0x11 -> 0x14 (PPC) in a working copy; keep the relocation tables.\n")
        out.write("- Core simulation modules: `NEWTON.c`, `SIMPCAR.c`, `WHEELS.c`, `POWTRAIN.c`, `AERO.c`, `COLLIDE.c`, `CATMULL.c`, `texload.c`, `sprite.c`, `clt*.c`, `game.c`, `ui.c`, `replay2.c` etc.\n")
    print(f"wrote {OUT_MD}")
    # JSON index
    idx = {
        "elf": ELF,
        "total_symbols": n,
        "func_count": len(funcs),
        "files": all_files,
        "funcs": [{"name":name,"addr":val,"size":sz,"shndx":shndx,"bind":bind} for name,val,sz,shndx,bind in sorted(funcs, key=lambda x: x[1])]
    }
    with open(OUT_JSON, 'w', encoding='utf-8') as jf:
        json.dump(idx, jf, indent=2)
    print(f"wrote {OUT_JSON}")
    # Quick stats
    # Group by file hint in name? Symbol names often include file? Actually Diab emits file symbols as STT_FILE
    # Let's also extract STT_FILE
    file_syms=[]
    for j in range(n):
        off=j*16
        st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack('>III BBH', sym_data[off:off+16])
        typ = st_info & 0xF
        if typ==4:
            name_end=str_data.find(b'\x00', st_name)
            sname=str_data[st_name:name_end].decode(errors='ignore') if st_name and name_end!=-1 else ''
            file_syms.append(sname)
    print(f"STT_FILE symbols: {file_syms[:20]}")

if __name__=="__main__":
    try:
        main()
    except (ValueError, struct.error, EOFError, OSError) as exc:
        raise SystemExit(f"extract_symbols: {exc}")
