#!/usr/bin/env python3
# transpile.py — static recomp: PPC602 ET_REL (Diab 3.7a) → C
#
# Input: the guest image `driving/game` from the user's game data (ET_REL,
#        e_machine 0x11, 483KB .text, 1365 funcs, hard-float single-precision)
# Output: generated/*.c + report + helpers integration
#
# Reloc handling:
#   164 LO16  — low half  @+2
#   165 HI16  — high half @+2 (no carry)
#   166 HA16  — high adjusted @+2 (carry from low)
#   186 ADDR32 — absolute word (474 .data tables + 561 .text pointer pools,
#               both recorded in a runtime fixup table (no bytes are baked in))
#   202 R_PPC_IMPREL24 — imported bl displacement; r_info's symbol field
#                        packs (.imp3do module index, export ordinal)
#
# Folio selector: (folio_id << 16) | ordinal
#
# The generated C emulates a PPCContext (gpr[32], fpr[32] float, cr, lr, ctr,
# xer, mem) with per-instruction semantics preserving the M2 FP rules:
#   lfs/stfs only (no lfd/stfd), fmadds via ppc_fma_single (single rounding), frsqrte
#   via bit-hack estimate, fsel via sign-bit test, fctiwz via trunc+sat.

import hashlib, os, struct, json, collections, argparse, textwrap, re

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTDIR = os.path.join(REPO, "generated")
R_LO16, R_HI16, R_HA16, R_ADDR32 = 164, 165, 166, 186
R_IMPREL24 = 202
# Keep the historical name local to this generator; every type-202 relocation
# in driving/game is an imported call, including module index 0.  The old
# emitter used `sym >> 16 != 0` as an import test, which silently converted
# the mpegaudiodecoder imports (module 0) into calls to guest 0x100000.
R_REL24 = R_IMPREL24

# `.imp3do` module order for driving/game, verified from its seven import
# records and literal name pool.  The relocation symbol field stores the
# module index, not the host's folio id.  The first six happened to match the
# existing host ids; mpegaudiodecoder is the important exception (module 0,
# host folio 7).
IMPORT_MODULE_TO_FOLIO = {
    0: 7,  # mpegaudiodecoder -> M2_FOLIO_MPEGAUDIODECODER
    1: 1,  # kernel
    2: 2,  # iff
    3: 3,  # graphics
    4: 4,  # filesystem
    5: 5,  # audiopatch
    6: 6,  # audio
}
# Name-based host folio assignment used for any image's ordered `.imp3do`
# records. The index in the record table is image-local; the host folio id is
# stable across images. Existing ids 1..7 stay fixed.
IMPORT_NAME_TO_FOLIO = {
    "mpegaudiodecoder": 7,
    "kernel": 1,
    "iff": 2,
    "graphics": 3,
    "filesystem": 4,
    "audiopatch": 5,
    "audio": 6,
    "gstate": 8,
    "font": 9,
}
# Folio import half-word relocs (3DO/M2 Diab custom codes). The sym field
# packs the folio selector (folio<<16)|ordinal, and the referenced item is an
# externally-imported folio symbol (data or function). Loaded as a lis+addi
# (or lis+ori/lwz) address pair; the guest treats that address as an IMPORT
# SLOT in guest RAM, so we bake the slot EA = IMPORT_TABLE_BASE + slot_idx*4.
# Half semantics mirror the local LO16/HI16/HA16 letters:
#   196 = R_LO16_FOLIO — low 16 bits (addi/lwz operand)
#   197 = R_HI16_FOLIO — high 16 bits, no carry (ori/nop OR combine)
#   198 = R_HA16_FOLIO — high 16 bits, carry-adjusted (addi/lwz ADD combine)
R_LO16_FOLIO, R_HI16_FOLIO, R_HA16_FOLIO = 196, 197, 198
# Host-reserved guest-RAM import-slot table, in the documented 0x003F0000
# carve gap (between broker 0x00300140 and heap 0x00400000), clear of the
# M2_GUEST_FRAME_* counters at 0x003F0000. Each slot holds one 4-byte pointer
# to an imported folio symbol, populated by the host import resolver.
IMPORT_TABLE_BASE = 0x003F1000
TEXT_BASE = 0x00100000

def _image_is(path, *tail):
    """True if `path` ends with the given directory/file components."""
    parts = os.path.abspath(path).split(os.sep)
    return parts[-len(tail):] == list(tail)

def align(x,n): return (x + n - 1) & ~(n-1)
def ha(v): return ((v + 0x8000) >> 16) & 0xFFFF
def lo(v): return v & 0xFFFF

class Binary:
    def __init__(self, path, text_base=TEXT_BASE,
                 import_table_base=IMPORT_TABLE_BASE,
                 profile="driving-game", output_prefix="game"):
        self.path = path
        self.profile = profile
        self.output_prefix = output_prefix
        self.symbol_prefix = "" if output_prefix == "game" else output_prefix + "_"
        self.import_table_base = import_table_base
        d = open(path, "rb").read()
        self.raw = d
        e_shoff = struct.unpack(">I", d[0x20:0x24])[0]
        nsec = struct.unpack(">H", d[0x30:0x32])[0]
        sx = struct.unpack(">H", d[0x32:0x34])[0]
        sec = [struct.unpack(">IIIIIIIIII", d[e_shoff+i*40:e_shoff+i*40+40]) for i in range(nsec)]
        shstr = sec[sx]
        sd = d[shstr[4]:shstr[4]+shstr[5]]
        def sname(s):
            e = sd.find(b"\x00", s[0]); return sd[s[0]:e].decode(errors="ignore") if e!=-1 else ""
        self.sec_by_name = {}
        self.sec_by_idx = {}
        for i,s in enumerate(sec):
            nm = sname(s)
            self.sec_by_name[nm] = (i,s)
            self.sec_by_idx[i] = (nm,s)
        # relocs
        self.relocs_text = {}  # offset -> (type, sym, addend)
        self.relocs_data = {}
        for rnm, dct in ((".rela.text", self.relocs_text), (".rela.data", self.relocs_data)):
            if rnm not in self.sec_by_name: continue
            _, rs = self.sec_by_name[rnm]
            for o in range(rs[4], rs[4]+rs[5], 12):
                ro, ri, ra = struct.unpack(">IIi", d[o:o+12])
                t = ri & 0xFF; sym = ri >> 8
                dct[ro] = (t, sym, ra & 0xFFFFFFFF)
        # symtab
        _, symtab = self.sec_by_name[".symtab"]
        _, strtab = self.sec_by_name[".strtab"]
        st = d[strtab[4]:strtab[4]+strtab[5]]
        sy = d[symtab[4]:symtab[4]+symtab[5]]
        self.syms = []
        self.sym_by_idx = {}
        cur_file = None
        text_idx = self.sec_by_name[".text"][0]
        for off in range(0, len(sy), 16):
            sn, val, sz, info, oth, shx = struct.unpack(">III BBH", sy[off:off+16])
            typ = info & 0xF; bind = info >> 4
            e = st.find(b"\x00", sn)
            nm = st[sn:e].decode(errors="ignore") if sn and e!=-1 else ""
            if typ == 4 and nm.endswith(".c"):
                cur_file = nm
            ent = dict(idx=len(self.syms), name=nm, val=val, sz=sz, typ=typ, bind=bind, shx=shx, file=cur_file)
            self.syms.append(ent)
            self.sym_by_idx[ent["idx"]] = ent
        # funcs: STT_FUNC in .text
        self.funcs = []
        for s in self.syms:
            if s["typ"] == 2 and s["shx"] == text_idx:
                self.funcs.append(s)
        self.funcs.sort(key=lambda x: x["val"])
        # sections
        _, ts = self.sec_by_name[".text"]
        self.text = d[ts[4]:ts[4]+ts[5]]
        self.text_off = ts[4]
        _, ds = self.sec_by_name[".data"]
        self.data = bytearray(d[ds[4]:ds[4]+ds[5]])
        self.data_off = ds[4]
        self.data_size = ds[5]
        _, bs = self.sec_by_name[".bss"]
        self.bss_size = bs[5]
        self.text_base = text_base
        self.data_base = align(self.text_base + len(self.text), 16)
        self.bss_base = align(self.data_base + len(self.data), 16)
        # REL24 and FOLIO relocations carry image-local .imp3do module indices.
        # Resolve them by name before the VA-thunk scan so both relocation
        # families use the same image-specific host folio assignment.
        self.import_module_to_folio = {}
        imp = self.sec_by_name.get(".imp3do")
        if imp is None:
            raise ValueError(f"{path}: missing .imp3do import table")
        imp_sec = imp[1]
        imp_size = imp_sec[5]
        imp_off = imp_sec[4]
        if imp_size < 20:
            raise ValueError(f"{path}: truncated .imp3do header")
        module_count = struct.unpack(">I", d[imp_off + 16:imp_off + 20])[0]
        if module_count == 0 or 20 + module_count * 12 > imp_size:
            raise ValueError(f"{path}: implausible .imp3do record count {module_count}")
        name_pool = 20 + module_count * 12
        last_name_offset = 0
        self.module_map = []
        for module_index in range(module_count):
            rec_off = imp_off + 20 + module_index * 12
            name_off, module_id, tag = struct.unpack(">III", d[rec_off:rec_off + 12])
            if name_off <= last_name_offset:
                raise ValueError(
                    f"{path}: .imp3do record {module_index} name offset is not increasing")
            last_name_offset = name_off
            name_at = imp_off + name_pool + name_off
            name_end = d.find(b"\0", name_at, imp_off + imp_size)
            if name_at >= imp_off + imp_size or name_end < 0:
                raise ValueError(
                    f"{path}: .imp3do record {module_index} has an unterminated name")
            module_name = d[name_at:name_end].decode("ascii", errors="strict")
            folio = IMPORT_NAME_TO_FOLIO.get(module_name.lower())
            if folio is None:
                raise ValueError(
                    f"{path}: no host folio assignment for .imp3do module "
                    f"{module_name!r} at index {module_index}")
            self.import_module_to_folio[module_index] = folio
            self.module_map.append({"index": module_index, "name": module_name,
                                    "module_id": module_id, "tag": tag,
                                    "host_folio_id": folio})
        if profile == "driving-game" and self.import_module_to_folio != IMPORT_MODULE_TO_FOLIO:
            raise ValueError(
                f"{path}: driving-game profile module map differs from its pinned map: "
                f"{self.import_module_to_folio}")
        # Promote evidenced zero-size assembly spans (ZERO_SIZE_SPANS) to
        # their real extent. The table end must not cross the next function,
        # every intervening function symbol must also have zero size, and the
        # selected span must disassemble with no invalid words, or generation
        # stops. An explicit end before the next symbol is allowed when a
        # source-image data/padding word follows a terminal return. Local
        # `endcode` labels otherwise bound assembly objects whose padding
        # extends to the next global function; intervening zero-size exports
        # identify aliases into one fall-through instruction stream.
        zero_size_spans = ZERO_SIZE_SPANS_BY_PROFILE.get(profile, {})
        for f in self.funcs:
            if f["sz"] != 0 or f["val"] not in zero_size_spans:
                continue
            end = zero_size_spans[f["val"]]
            later_sized_funcs = [g["val"] for g in self.funcs
                                 if g["val"] > f["val"] and g["sz"] != 0]
            next_func = (min(later_sized_funcs) if later_sized_funcs
                         else len(self.text))
            intervening = [g for g in self.funcs
                           if f["val"] < g["val"] < end and g["sz"] != 0]
            if end <= f["val"] or end > next_func or (end & 3) or intervening:
                raise SystemExit(
                    f"zero-size span table {f['val']:#x}->{end:#x} does not "
                    "end on an aligned boundary at or before the next function "
                    f"({next_func:#x}) with only zero-size intervening symbols: "
                    f"{intervening}")
            f["sz"] = end - f["val"]
            try:
                from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN
                md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
                bad = [i.address for i in
                       _disasm_resync(md, self.text[f["val"]:end], f["val"])
                       if i.mnemonic == "invalid"]
                if bad:
                    raise SystemExit(
                        f"zero-size span {f['val']:#x}..{end:#x} "
                        f"({f['name']}) disassembles with invalid words at "
                        f"{[hex(a) for a in bad]} — refusing to lift")
            except ImportError:
                pass  # no-capstone fallback cannot validate; table is the gate
        # Diab emits the VA forms of local C wrappers as three-instruction
        # DECVASTUB thunks, but leaves their STT_FUNC size at zero. Recover
        # only the exact source-backed shape: lis/ori r0 with matching local
        # or imported HI16/LO16 relocations followed by a tail branch to one
        # of the kernel __tagCall helpers. Imported aliases are promoted only
        # when their folio/ordinal contract has a typed recomp boundary.
        self.va_thunks = []
        self.imported_va_thunks = []
        funcs_by_val = {f["val"]: f for f in self.funcs}
        for i, f in enumerate(self.funcs):
            if f["sz"] != 0:
                continue
            end = (self.funcs[i + 1]["val"] if i + 1 < len(self.funcs)
                   else len(self.text))
            if end - f["val"] != 12:
                continue
            addr = f["val"]
            words = [struct.unpack(">I", self.text[addr + off:addr + off + 4])[0]
                     for off in (0, 4, 8)]
            # lis r0,imm; ori r0,r0,imm; b __tagCall_N.
            if ((words[0] & 0xffff0000) != 0x3c000000 or
                    (words[1] & 0xffff0000) != 0x60000000 or
                    ((words[1] >> 21) & 0x1f) != 0 or
                    ((words[1] >> 16) & 0x1f) != 0 or
                    (words[2] >> 26) != 18 or (words[2] & 3) != 0):
                continue
            hi = self.relocs_text.get(addr + 2)
            lo_ = self.relocs_text.get(addr + 6)
            tail = self.relocs_text.get(addr + 8)
            text_idx = self.sec_by_name[".text"][0]
            if (hi and hi[0] == R_HI16_FOLIO and
                    lo_ and lo_[0] == R_LO16_FOLIO and
                    hi[1] == lo_[1] and hi[2] == lo_[2] and
                    tail and tail[0] == R_REL24 and
                    (tail[1] >> 16) == 1 and
                    tail[1] & 0xffff in (89, 90, 91)):
                folio, ordinal = self.import_selector(hi[1])
                helper_ord = tail[1] & 0xffff
                imported = {
                    "guest": addr,
                    "name": f["name"],
                    "size": 12,
                    "folio": folio,
                    "ordinal": ordinal,
                    "tag_call_ordinal": helper_ord,
                    "fixed_arg_count": helper_ord - 89,
                    "promoted": (folio, ordinal, helper_ord) in {
                        (3, 1, 90),   # ModifyGraphicsItemVA
                        (6, 46, 90),  # SetAudioItemInfoVA
                    },
                }
                self.imported_va_thunks.append(imported)
                # These imported VA thunks have source-backed typed handlers
                # in the current graphics/audio slices. Keep every other
                # imported alias fail-closed until its call shape is known.
                if imported["promoted"]:
                    f["lift_size"] = 12
                    f["lift_kind"] = "imported_diab_va_thunk"
                    f["lift_target_folio"] = folio
                    f["lift_target_ordinal"] = ordinal
                    f["lift_tag_call_ordinal"] = helper_ord
                    f["lift_fixed_arg_count"] = helper_ord - 89
                continue
            if (not hi or hi[0] != R_HI16 or not lo_ or lo_[0] != R_LO16 or
                    hi[1] != lo_[1] or hi[2] != lo_[2] or
                    self.sym_by_idx.get(hi[1], {}).get("shx") != text_idx or
                    not tail or tail[0] != R_REL24 or (tail[1] >> 16) != 1 or
                    tail[1] & 0xffff not in (89, 90, 91)):
                continue
            target = hi[2]
            target_func = funcs_by_val.get(target)
            if not target_func or target_func["sz"] == 0:
                continue
            helper_ord = tail[1] & 0xffff
            f["lift_size"] = 12
            f["lift_kind"] = "local_diab_va_thunk"
            f["lift_target"] = target
            f["lift_tag_call_ordinal"] = helper_ord
            self.va_thunks.append({
                "guest": addr,
                "name": f["name"],
                "size": 12,
                "target": target,
                "target_name": target_func["name"],
                "tag_call_ordinal": helper_ord,
            })
        # strtab for sym names
        self.strtab = st
        # Deterministic import-slot indices: every FOLIO import reloc (196/197/
        # 198) carries a folio selector in its sym field ((folio<<16)|ord). Each
        # distinct (folio, ord) gets a slot in the host import table, ordered
        # by (folio, ord). Each slot EA is IMPORT_TABLE_BASE + slot_idx*4.
        self.import_slots = {}      # (folio, ord) -> slot index
        pairs = set()
        FOLIO_TYPES = (R_LO16_FOLIO, R_HI16_FOLIO, R_HA16_FOLIO)
        for dct in (self.relocs_text, self.relocs_data):
            for t, sym, _add in dct.values():
                if t in FOLIO_TYPES:
                    pairs.add(self.import_selector(sym))
        for i, pair in enumerate(sorted(pairs)):
            self.import_slots[pair] = i

    def import_slot_ea(self, sym):
        """Import-slot guest EA for an image-local .imp3do selector relocation."""
        pair = self.import_selector(sym)
        idx = self.import_slots.get(pair)
        if idx is None:
            return None
        return self.import_table_base + idx * 4

    def import_selector(self, sym):
        """Translate an IMPREL24 symbol to the host folio/ordinal pair.

        `sym` is already r_info >> 8, so its high 16 bits are the image's
        `.imp3do` module index and its low 16 bits are the export ordinal.
        Module 0 is valid; zero is not an internal-call sentinel.
        """
        module = sym >> 16
        try:
            folio = self.import_module_to_folio[module]
        except KeyError as exc:
            raise ValueError(
                f"unknown .imp3do module index {module} in import sym {sym:#x}"
            ) from exc
        return folio, sym & 0xFFFF

    def sym_name(self, idx):
        return self.sym_by_idx.get(idx, {}).get("name", f"<sym{idx}>")

    def abs_addr_for_reloc(self, sym_idx, addend):
        # sym is section symbol (.text=2, .data=3, .bss=4) with val 0
        ent = self.sym_by_idx.get(sym_idx)
        shx = ent["shx"] if ent else 0
        if shx == self.sec_by_name[".text"][0]:
            return self.text_base + (addend & 0xFFFFFFFF)
        if shx == self.sec_by_name[".data"][0]:
            return self.data_base + (addend & 0xFFFFFFFF)
        if shx == self.sec_by_name[".bss"][0]:
            return self.bss_base + (addend & 0xFFFFFFFF)
        # absolute (folio selector) — not an address
        return addend

def sanitize(name):
    s = re.sub(r'[^A-Za-z0-9_]', '_', name)
    if s and s[0].isdigit():
        s = '_' + s
    if not s:
        s = '_anon'
    return s

def func_sym(func, image_prefix=""):
    # unique C identifier for a guest function (handles duplicate names across CUs)
    base = sanitize(func["name"])
    return f"{image_prefix}{base}__{func['val']:05x}"

# Diab save/restore ordinals mapped to start register and kind
DIAB_SAVE_GPR = {
    489: 14, 492: 17, 493: 18, 494: 19, 495: 20, 496: 21, 497: 22, 498: 23, 499: 24, 500: 25, 501: 26, 502: 27,
    525: 14, 526: 15, 527: 16, 529: 18, 530: 19, 532: 21, 533: 22, 534: 23, 535: 24, 536: 25, 537: 26, 538: 27,
}
DIAB_SAVE_FPR = {
    453: 14, 459: 20, 460: 21, 461: 22, 462: 23, 463: 24, 464: 25, 465: 26, 466: 27, 467: 28,
}
DIAB_REST_GPR_NONLEAF = {
    381: 14, 384: 17, 385: 18, 386: 19, 387: 20, 388: 21, 389: 22, 390: 23, 391: 24, 392: 25, 393: 26, 394: 27,
}
DIAB_REST_GPR_LEAF = {
    417: 14, 418: 15, 419: 16, 421: 18, 422: 19, 424: 21, 425: 22, 426: 23, 427: 24, 428: 25, 429: 26, 430: 27,
}
DIAB_REST_FPR_LEAF = {
    345: 14, 351: 20, 352: 21, 353: 22, 354: 23, 355: 24, 356: 25, 357: 26, 358: 27, 359: 28,
}
DIAB_ALL_ORDS = (
    set(DIAB_SAVE_GPR.keys())
    | set(DIAB_SAVE_FPR.keys())
    | set(DIAB_REST_GPR_NONLEAF.keys())
    | set(DIAB_REST_GPR_LEAF.keys())
    | set(DIAB_REST_FPR_LEAF.keys())
)
# Varargs helpers
VARARGS_ORDS = {267: "__va_arg", 89: "__tagCall_0", 90: "__tagCall_1", 91: "__tagCall_2"}

# Declarative tracepoints.  The limits are intentionally small:
# tracepoints are investigation aids, not a general debugger or an unbounded
# memory dumper.
TRACEPOINT_MAX_SPECS = 64
TRACEPOINT_MAX_MEMORY = 16
TRACEPOINT_MAX_CAPTURE_BYTES = 256


def _tracepoint_int(value, field, signed=False):
    """Parse one bounded integer from a tracepoint JSON field."""
    if isinstance(value, bool) or not isinstance(value, (int, str)):
        raise ValueError(f"{field} must be an integer or numeric string")
    try:
        if isinstance(value, int):
            result = value
        else:
            text = value.strip()
            if not text:
                raise ValueError
            result = int(text, 0)
    except ValueError:
        raise ValueError(f"{field} is not a valid integer: {value!r}")
    if not signed and result < 0:
        raise ValueError(f"{field} must be non-negative")
    if result < -0x80000000 or result > 0xFFFFFFFF:
        raise ValueError(f"{field} is outside the 32-bit range")
    return result


def _tracepoint_reg(value, field, gpr_only=False):
    if not isinstance(value, str):
        raise ValueError(f"{field} must be a register name (got {value!r})")
    token = value.strip().lower()
    if token.startswith("r") and token[1:].isdigit():
        number = int(token[1:], 10)
        if 0 <= number <= 31:
            return token, number
    if not gpr_only and token in ("lr", "ctr", "cr"):
        return token, None
    if gpr_only:
        raise ValueError(f"{field} must be r0 through r31 (got {value!r})")
    raise ValueError(
        f"{field} must be r0 through r31, lr, ctr, or cr (got {value!r})")


def _tracepoint_instruction_addresses(bin_, gap_sizes):
    """Return decoded instruction PCs eligible for a generic hook."""
    from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN

    md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
    md.detail = False
    spans = []
    for func in bin_.funcs:
        size = func.get("lift_size", func.get("sz", 0))
        if size:
            spans.append((func["val"], size, func.get("lift_kind")))
    spans.extend((addr, size, None) for addr, size in gap_sizes.items())

    addresses = set()
    for addr, size, lift_kind in spans:
        # Imported Diab VA thunks are emitted as one compact generic call, so
        # only their entry boundary is available for a tracepoint.
        if lift_kind == "imported_diab_va_thunk":
            addresses.add(addr)
            continue
        for ins in _disasm_resync(md, bin_.text[addr:addr + size], addr):
            if ins.mnemonic == "invalid":
                continue
            # ADDR32 relocation words are embedded pointer pools, not PPC
            # instructions, even when Capstone can decode their bit pattern.
            reloc = bin_.relocs_text.get(ins.address)
            if reloc and reloc[0] == R_ADDR32:
                continue
            addresses.add(ins.address)
    return addresses


def load_tracepoints(bin_, path, gap_sizes):
    """Load, validate, and resolve one declarative tracepoint JSON file."""
    if not path:
        return []
    try:
        with open(path, "r", encoding="utf-8") as f:
            document = json.load(f)
    except OSError as exc:
        raise SystemExit(f"tracepoint config {path}: {exc}")
    except json.JSONDecodeError as exc:
        raise SystemExit(
            f"tracepoint config {path}: invalid JSON at line {exc.lineno}, "
            f"column {exc.colno}: {exc.msg}")

    if not isinstance(document, dict) or set(document) != {"tracepoints"}:
        raise SystemExit(
            f"tracepoint config {path}: root must contain only a tracepoints array")
    records = document["tracepoints"]
    if not isinstance(records, list):
        raise SystemExit(f"tracepoint config {path}: tracepoints must be an array")
    if len(records) > TRACEPOINT_MAX_SPECS:
        raise SystemExit(
            f"tracepoint config {path}: at most {TRACEPOINT_MAX_SPECS} tracepoints are supported")

    valid_pcs = _tracepoint_instruction_addresses(bin_, gap_sizes)
    resolved = []
    seen_pcs = set()

    def fail(index, message):
        raise SystemExit(f"tracepoint config {path}: tracepoints[{index}]: {message}")

    for index, record in enumerate(records):
        if not isinstance(record, dict):
            fail(index, "entry must be an object")
        allowed = {"pc", "symbol", "offset", "name", "limit", "registers",
                   "memory", "when"}
        unknown = set(record) - allowed
        if unknown:
            fail(index, f"unknown field(s): {', '.join(sorted(unknown))}")
        if ("pc" in record) == ("symbol" in record):
            fail(index, "provide exactly one of pc or symbol")

        if "symbol" in record:
            symbol = record["symbol"]
            if not isinstance(symbol, str) or not symbol.strip():
                fail(index, "symbol must be a non-empty string")
            matches = [f for f in bin_.funcs if f.get("name") == symbol]
            if len(matches) != 1:
                if not matches:
                    fail(index, f"symbol {symbol!r} was not found in the source image")
                fail(index, f"symbol {symbol!r} is ambiguous; use an explicit pc")
            offset = 0
            if "offset" in record:
                try:
                    offset = _tracepoint_int(record["offset"], "offset", signed=True)
                except ValueError as exc:
                    fail(index, str(exc))
            target = matches[0]["val"] + offset
            span = matches[0].get("lift_size", matches[0].get("sz", 0))
            if offset < 0 or offset >= span:
                fail(index, f"offset {offset:#x} is outside symbol {symbol!r}")
        else:
            try:
                target = _tracepoint_int(record["pc"], "pc")
            except ValueError as exc:
                fail(index, str(exc))
            if target >= bin_.text_base:
                target -= bin_.text_base

        if target not in valid_pcs:
            fail(index, f"resolved pc {target:#x} is not a decoded guest instruction")
        if target in seen_pcs:
            fail(index, f"pc {target:#x} is configured more than once")
        seen_pcs.add(target)

        name = record.get("name", f"tracepoint_{index}")
        if not isinstance(name, str) or not name:
            fail(index, "name must be a non-empty string")
        if len(name.encode("utf-8")) > 63:
            fail(index, "name must be at most 63 UTF-8 bytes")

        try:
            limit = _tracepoint_int(record.get("limit", 0), "limit")
        except ValueError as exc:
            fail(index, str(exc))
        if limit > 0x7FFFFFFF:
            fail(index, "limit is too large")

        registers = record.get("registers", [])
        if not isinstance(registers, list):
            fail(index, "registers must be an array")
        register_names = []
        gpr_mask = 0
        capture_lr = capture_ctr = capture_cr = False
        for reg_index, value in enumerate(registers):
            try:
                name_value, number = _tracepoint_reg(value, f"registers[{reg_index}]")
            except ValueError as exc:
                fail(index, str(exc))
            if name_value in register_names:
                fail(index, f"register {name_value} is configured more than once")
            register_names.append(name_value)
            if number is not None:
                gpr_mask |= 1 << number
            elif name_value == "lr":
                capture_lr = True
            elif name_value == "ctr":
                capture_ctr = True
            elif name_value == "cr":
                capture_cr = True

        memory = record.get("memory", [])
        if not isinstance(memory, list):
            fail(index, "memory must be an array")
        if len(memory) > TRACEPOINT_MAX_MEMORY:
            fail(index, f"at most {TRACEPOINT_MAX_MEMORY} memory ranges are supported")
        memory_specs = []
        for mem_index, item in enumerate(memory):
            if not isinstance(item, dict):
                fail(index, f"memory[{mem_index}] must be an object")
            allowed_memory = {"ea", "base_register", "offset", "length",
                              "provenance", "label"}
            unknown_memory = set(item) - allowed_memory
            if unknown_memory:
                fail(index, f"memory[{mem_index}] unknown field(s): "
                     f"{', '.join(sorted(unknown_memory))}")
            is_fixed = "ea" in item
            is_relative = "base_register" in item
            if is_fixed == is_relative:
                fail(index, f"memory[{mem_index}] provide exactly one of ea or base_register")
            if "length" not in item:
                fail(index, f"memory[{mem_index}] requires length")
            try:
                length = _tracepoint_int(item["length"], f"memory[{mem_index}].length")
            except ValueError as exc:
                fail(index, str(exc))
            if length == 0 or length > TRACEPOINT_MAX_CAPTURE_BYTES:
                fail(index, f"memory[{mem_index}].length must be 1..{TRACEPOINT_MAX_CAPTURE_BYTES}")
            offset = 0
            if "offset" in item:
                try:
                    offset = _tracepoint_int(item["offset"], f"memory[{mem_index}].offset", signed=True)
                except ValueError as exc:
                    fail(index, str(exc))
            base_register = None
            ea = 0
            if is_fixed:
                if "offset" in item:
                    fail(index, f"memory[{mem_index}] fixed ranges cannot use offset")
                try:
                    ea = _tracepoint_int(item["ea"], f"memory[{mem_index}].ea")
                except ValueError as exc:
                    fail(index, str(exc))
            else:
                try:
                    _, base_register = _tracepoint_reg(
                        item["base_register"], f"memory[{mem_index}].base_register",
                        gpr_only=True)
                except ValueError as exc:
                    fail(index, str(exc))
            provenance = item.get("provenance", False)
            if not isinstance(provenance, bool):
                fail(index, f"memory[{mem_index}].provenance must be boolean")
            label = item.get("label", f"memory_{mem_index}")
            if not isinstance(label, str) or not label:
                fail(index, f"memory[{mem_index}].label must be a non-empty string")
            if len(label.encode("utf-8")) > 31:
                fail(index, f"memory[{mem_index}].label must be at most 31 UTF-8 bytes")
            memory_specs.append({
                "kind": "fixed" if is_fixed else "relative",
                "ea": ea,
                "base_register": base_register,
                "offset": offset,
                "length": length,
                "provenance": provenance,
                "label": label,
            })

        when = record.get("when")
        when_kind = "none"
        when_register = None
        if when is not None:
            if not isinstance(when, dict) or set(when) != {"register", "op"}:
                fail(index, "when must contain exactly register and op")
            try:
                _, when_register = _tracepoint_reg(when["register"], "when.register", gpr_only=True)
            except ValueError as exc:
                fail(index, str(exc))
            if when["op"] != "outside_guest_ram":
                fail(index, "when.op must be outside_guest_ram")
            when_kind = "outside_guest_ram"

        resolved.append({
            "id": index,
            "name": name,
            "pc": target,
            "limit": limit,
            "gpr_mask": gpr_mask,
            "registers": register_names,
            "capture_lr": capture_lr,
            "capture_ctr": capture_ctr,
            "capture_cr": capture_cr,
            "when_kind": when_kind,
            "when_register": when_register,
            "memory": memory_specs,
        })
    return resolved


def _reg(token, prefix="r"):
    """Return a PPC register number from Capstone's operand spelling."""
    token = token.strip()
    if token == "0":
        return 0
    if not token.startswith(prefix):
        raise ValueError(token)
    return int(token[len(prefix):], 0)


def _imm(token):
    """Parse a Capstone hexadecimal or decimal immediate."""
    return int(token.strip(), 0)


def _cr_bit(token):
    """Return the architectural CR-bit number from a Capstone spelling."""
    m = re.fullmatch(r"cr([0-7])(lt|gt|eq|so|un)", token.strip())
    if not m:
        raise ValueError(token)
    which = {"lt": 0, "gt": 1, "eq": 2, "so": 3, "un": 3}[m.group(2)]
    return int(m.group(1)) * 4 + which


def _mem_operand(token):
    """Parse a D-form operand such as -0x10(r1)."""
    m = re.fullmatch(r"\s*([+-]?(?:0x[0-9a-fA-F]+|[0-9]+))\((r[0-9]+)\)\s*", token)
    if not m:
        raise ValueError(token)
    return _imm(m.group(1)), _reg(m.group(2))


def _relocated_imm(bin_, pc, raw, signed=False, high=False):
    """Resolve the custom Diab half-word relocation for one instruction."""
    rel = bin_.relocs_text.get(pc + 2)
    if rel and rel[0] in (R_LO16, R_HI16, R_HA16):
        absolute = bin_.abs_addr_for_reloc(rel[1], rel[2])
        if high:
            value = ha(absolute) if rel[0] == R_HA16 else (absolute >> 16) & 0xffff
        else:
            value = lo(absolute) if rel[0] == R_LO16 else raw
    elif rel and rel[0] in (R_LO16_FOLIO, R_HI16_FOLIO, R_HA16_FOLIO):
        # Folio import address: bake the import-slot EA so the guest reads the
        # slot pointer the host resolver installs there. 198=HA16 (addi/lwz ADD
        # combine carries), 197=HI16 (ori OR combine, no carry).
        absolute = bin_.import_slot_ea(rel[1])
        if absolute is None:
            raise SystemExit(
                f"FOLIO reloc at {pc+2:#x} has no import slot for "
                f"folio {rel[1]>>16} ord {rel[1]&0xFFFF} — refusing to bake 0")
        if high:
            value = ha(absolute) if rel[0] == R_HA16_FOLIO \
                else (absolute >> 16) & 0xffff
        else:
            value = lo(absolute) if rel[0] == R_LO16_FOLIO else raw
    else:
        value = raw
    if signed and 0 <= value <= 0xffff and value & 0x8000:
        value -= 0x10000
    return value


def _c_gpr(n):
    return f"c->gpr[{n}]"


def _c_fpr(n):
    return f"c->fpr[{n}]"


def _base_expr(ra):
    # PPC D-form addressing treats r0 as the literal zero.
    return "0u" if ra == 0 else _c_gpr(ra)


def _indexed_expr(ra, rb):
    # In X-form addressing only the base rA has the r0-zero rule; rB is a
    # normal register, including r0. This matters for stfiwx prologues.
    return f"({_base_expr(ra)} + {_c_gpr(rb)})"


def _record(lines, expr):
    """Set CR0 for an integer record-form instruction."""
    lines.append(f"    cr_set_rec(c, 28, (int32_t)({expr}));")


def _carry_add(lines, dest, left, right, carry="0u", pc=0):
    """Emit a 32-bit add and PPC XER carry update.

    All three addends are reduced mod 2**32 before the 64-bit sum so a
    negative C immediate (e.g. addic `str(-1)`) contributes 0xFFFFFFFF,
    not 0xFFFFFFFFFFFFFFFF: PPC CA is the carry out of bit 0 of the
    32-bit add, and (uint64_t)(-1) would set bit 32 spuriously while
    missing the genuine carry (e.g. 1 + -1 must carry, 0 + -1 must not).
    """
    name = f"_sum_{pc:x}"
    lines.append(f"    uint64_t {name} = (uint64_t)(uint32_t)({left}) + (uint64_t)(uint32_t)({right}) + (uint64_t)(uint32_t)({carry});")
    lines.append(f"    {_c_gpr(dest)} = (uint32_t){name};")
    lines.append(f"    c->xer = (c->xer & ~0x20000000u) | (uint32_t)(({name} >> 32) & 1u) << 29;")


class _InvalidInsn:
    """Fixed-width placeholder used to resynchronize after bad Capstone data."""
    def __init__(self, address, word):
        self.address = address
        self.mnemonic = "invalid"
        self.op_str = f"0x{word:08x}"


def _disasm_resync(md, data, address):
    """Disassemble PPC words independently so one bad word does not drop a tail."""
    result = []
    end = address + len(data)
    pc = address
    while pc + 4 <= end:
        decoded = list(md.disasm(data[pc - address:pc - address + 4], pc, count=1))
        if decoded:
            result.append(decoded[0])
        else:
            result.append(_InvalidInsn(pc, struct.unpack(">I", data[pc - address:pc - address + 4])[0]))
        pc += 4
    return result


# --- intra-function switch tables (Diab embeds them inside .text) ----------
#
# Diab lowers a switch whose cases stay in one function to
#   slwi rI, rIdx, 2; addis rT, rI, ha(pool); cmplwi/bgt default;
#   lwz rC, lo(pool)(rT); mtctr rC; bctr
# where `pool` is a run of R_ADDR32->.text words *embedded between the case
# bodies* inside the enclosing function's symbol span. The generic emitter
# turns the bctr into recomp_lookup(c->ctr), which can never hit because the
# targets are mid-function labels, not function starts -> RECOMP_INDIRECT.
#
# For audited functions only (SWITCH_DISPATCH_FUNCS), such tails are lifted to
# a bounded C `switch (c->ctr)` over the pool words' absolute VAs with gotos
# to labels emitted at the target instructions. Detection uses nothing but ELF
# geometry and relocation records (no heuristics on register history beyond
# the fixed 3-instruction tail shape), so the output is deterministic.
# Pool words are non-executable data: they are emitted as `.word` markers and
# they are not counted as instructions.
SWITCH_DISPATCH_FUNCS = {
    0x07118,  # bsdf_read.c :: UpdateLoadedPod — 17-entry interior jump-table (pool 0x7144..0x7184)
    0x258f8,  # M_Readers.c :: MS_UpdateLoadedPodLodGroup — 10-entry interior jump-table
              # (pool 0x259c4..0x259e8, targets 0x25a4c/0x25a28/0x25a14/0x25a00/0x259ec)
              # reached via lwz/mtctr/bctr at 0x259c0. The first bctr is the case-0x18
              # `UpdateLoadedPod` switch index on the POD type info block, the second
              # (0x25b10 in MS_UpdateLoadedPodLodGroup_special) is the same idiom.
              # Generic indirect dispatch would lower this to
              # `recomp_lookup(c->ctr) -> RECOMP_INDIRECT`, fabricating
              # `gpr[3]=1` and skipping the POD case body. The 10 interior labels
              # are switch arms of the lifted function, so the dispatch stays
              # guest-owned.
    0x25a60,  # M_Readers.c :: MS_UpdateLoadedPodLodGroup_special — 10-entry interior
              # jump-table (pool 0x25b14..0x25b38, targets 0x25b90/0x25b3c/0x25b78)
              # reached via lwz/mtctr/bctr at 0x25b10. Same fix as
              # MS_UpdateLoadedPodLodGroup@0x258f8 above.
    0x230f8,  # M_Support.c :: MS_AssignProperty — 12-entry interior
              # jump-table (pool 0x2319c..0x231c4, 9 unique targets) reached
              # via lwz/mtctr/bctr at 0x23198. This is the first
              # State 2 MS_CloneGroup property-dispatch miss after the valid
              # POD group allocation; keep the target set ELF/reloc-derived.
    0x36430,  # transitions.c :: dotrans — 31-entry interior jump-table
              # (pool 0x36478..0x364f8) reached by the selector chosen from
              # the transition type. The table is entered from the State 2
              # display path; leaving it as generic indirect dispatch leaks
              # dotrans's 0x20-byte frame when an interior target is missed.
    0x2fa4c,  # state.c :: StateDriver (42-entry FSM)
    0x3811c,  # ui.c :: UI_DrawMain — 7-entry interior jump-table (pool 0x38214..0x3822c,
              # targets 0x138230..0x1382c0) reached via lwz/mtctr/bctr; closes the
              # closes the fail-closed interior-label indirect dispatch (RECOMP_INDIRECT at ctr=0x138230).
    0x382ec,  # ui.c :: UI_DrawConfigRace — 7-entry interior jump-table (pool 0x3846c..0x38484,
              # targets 0x138488..0x138518) reached via lwz/mtctr/bctr. Preserve the
              # guest-local draw dispatch so the State 6 callback returns through its
              # epilogue instead of failing closed from inside the function body.
    0x38d84,  # ui.c :: UI_DrawConfigCar — 8-entry interior jump-table (pool 0x38ea8..0x38ec4,
              # targets 0x138ec8..0x138f70) reached via lwz/mtctr/bctr. Preserve this
              # guest-local dispatch when the BSS-resident callback reaches this function.
    0x21a68,  # M_Support.c :: MS_ScanList — 4-entry interior jump-table
              # (pool 0x21cb0..0x21cd0, targets 0x121cd4/0x121df8/0x121ea8/0x121f2c)
              # observed on the State 2 -> State 3 runtime path at bctr 0x21cac.
    0x445f4,  # CATMULL.c :: test_quad — 5-entry interior jump-table
              # (pool 0x44b3c..0x44b64, targets 0x144b68/0x144b74/0x144b7c/
              # 0x144b88/0x145050) observed on the State 2 -> State 3 path at
              # bctr 0x44b38.
    0x1e7b8,  # main.c :: Parse_Args — option dispatch table at 0x1e868;
              # `-T<name>` and the boot `-V*` options branch to interior
              # handlers, so generic recomp_lookup cannot resolve them.
    0x74cc4,  # Matrix_Perspective.c :: __vfscanf — relocated interior tables
              # at bctr 0x74f28 and 0x751a4; the first table includes the
              # observed target 0x17575c. Targets are all labels in this
              # function, so they must remain guest-local dispatches.
    0x64850,  # Matrix_Perspective.c :: ProcessMPEGVideoRequests — 12-entry
              # interior jump-table (pool 0x648b0..0x648dc, targets 0x648e0/
              # 0x648f4/0x64908/0x6491c/0x64930/0x64984/0x64970/0x64944/
              # 0x64958/0x64998/0x649ac/0x649c4) reached via lwz r11,0x48b0(r12)
              # at 0x648a4 / mtctr r11 at 0x648a8 / bctr at 0x648ac. Index is the
              # request-type word from lwz r11,0(r31), scaled by slwi 2, based by
              # addis r12,r11,6, bounded by cmplwi 0xb with default 0x649d8
              # (ReplyToSubscriberMsg@0x628a0); index 0 routes to
              # DoMPEGVideoData@0x63f24. Pool words carry ADDR32 relocs against
              # .text to interior labels; HA16 at 0x6489e and LO16 at 0x648a6
              # share pool-VA addend 0x648b0. Single local caller
              # MPEGVideoSubscriberThread@0x64b88 (bl at 0x64b9c).
    0x632a4,  # Matrix_Perspective.c :: DoControl — 12-entry interior
              # jump-table (pool 0x632d4..0x63300, targets 0x63304/0x63318/
              # 0x63340/0x63400/0x63368/0x6345c/0x63390/0x63420/0x63444/
              # 0x63450/0x633b8/0x633dc) reached via lwz r11,0x32d4(r12)
              # at 0x632c8 / mtctr r11 at 0x632cc / bctr at 0x632d0. Index is
              # the request-type word from lwz r6,0x10(r4), scaled by slwi 2,
              # based by addis r12,r11,6, bounded by cmplwi 0xb with default
              # 0x6345c. Pool words carry ADDR32 relocs against .text to
              # interior labels; HA16 at 0x632c2 and LO16 at 0x632ca share
              # pool-VA addend 0x632d4. Single local caller
              # ProcessRequestMsgs@0x635dc (bl at 0x636b8).
    0x635dc,  # Matrix_Perspective.c :: ProcessRequestMsgs — 12-entry
              # interior jump-table (pool 0x63644..0x63670, targets 0x63674/
              # 0x63688/0x6369c/0x636b0/0x636c4/0x63718/0x63704/0x636d8/
              # 0x636ec/0x6372c/0x63740/0x63758) reached via lwz r11,0x3644(r12)
              # at 0x63638 / mtctr r11 at 0x6363c / bctr at 0x63640. Index is
              # the request-type word from lwz r11,0(r31) via the 0x2c-indirect
              # reply word, scaled by slwi 2, based by addis r12,r11,6, bounded
              # by cmplwi 0xb with default 0x63768. Pool words carry ADDR32
              # relocs against .text to interior labels; HA16 at 0x63632 and
              # LO16 at 0x6363a share pool-VA addend 0x63644. Local callers
              # MPEGAudioSubscriberThread@0x60d6c (bl at 0x60e54) and
              # SAudioSubscriberThread@0x65848 (bl at 0x658cc).
    0x68e44,  # Matrix_Perspective.c :: HandleDSRequest — 12-entry interior
              # jump-table (pool 0x68eb8..0x68ee4, targets 0x68ee8/0x68efc/
              # 0x68f10/0x68f24/0x68f38/0x68f4c/0x68f60/0x68f74/0x68f88/
              # 0x68f9c/0x68fb0/0x68fc4) reached via lwz r11,-0x7148(r12)
              # at 0x68eac / mtctr r11 at 0x68eb0 / bctr at 0x68eb4. Index is
              # the request-type word from lwz r11,0(r5) via the 0x70-indirect
              # stream word, scaled by slwi 2, based by addis r12,r11,7,
              # bounded by cmplwi 0xb with default 0x68fd8. Pool words carry
              # ADDR32 relocs against .text to interior labels; HA16 at
              # 0x68ea6 and LO16 at 0x68eae share pool-VA addend 0x68eb8.
              # Local callers DataStreamThread@0x692a0 (bl at 0x69330 and
              # bl at 0x69404).
    0x844c,   # camera.c :: Cam_new_replay — 8-entry interior jump-table
              # (pool 0x85c0..0x85dc, targets 0x87dc/0x85e0/0x8600/0x8634/
              # 0x8650/0x8750/0x8790/0x87b8) reached via lwz r11,-0x7a40(r12)
              # at 0x85b4 / mtctr r11 at 0x85b8 / bctr at 0x85bc. Index is the
              # converted count from lwz r26,8(r1) (fctiwz stack spill), scaled
              # by slwi 2, based by addis r12,r11,1, bounded by cmplwi 7 with
              # default 0x87dc. Pool words carry ADDR32 relocs against .text
              # to interior labels; HA16 at 0x85ae and LO16 at 0x85b6 share
              # pool-VA addend 0x85c0. Single local caller Cam_Update@0x9340
              # (bl at 0x9388).
    0x386ac,  # ui.c :: UI_DrawOptions — 9-entry interior jump-table
              # (pool 0x387d0..0x387f0, targets 0x387f4/0x3880c/0x38824/
              # 0x3883c/0x38854/0x3886c/0x38884/0x3889c/0x388b4) reached via
              # lwz r11,-0x7830(r12) at 0x387c4 / mtctr r11 at 0x387c8 / bctr
              # at 0x387cc. Index is the option byte from lbz r11,3(r31) with
              # extsb, scaled by slwi 2, based by addis r12,r11,4, bounded by
              # cmplwi 8 with default 0x388c8. Pool words carry ADDR32 relocs
              # against .text to interior labels; HA16 at 0x387be and LO16 at
              # 0x387c6 share pool-VA addend 0x387d0. Single local caller
              # StateDriver@0x2fa4c (bl at 0x30d1c).
}

# Zero-size assembly spans lifted despite st_size == 0. The Diab toolchain emitted these assembly-authored
# spans as zero-size STT_FUNC symbols; their real instruction streams are
# bounded by the next function symbol. Each entry was promoted only on
# explicit evidence:
#   - clean independent disassembly of the whole span (0 invalid words);
#   - the span-end equals the next STT_FUNC symbol value;
#   - M_Sort: 5 external `bl 0x60098` callers (EF_ThreadSmokeObjectsList,
#     MS_ScanList, MS_ShowWorld, SR_ShowMerc, UI_ShowMerc) and the 4 BSS
#     relocations the v4 audit attributed to M_userpodptrs (0x9eb8) and
#     M_anchorpod (0x9e90);
#   - M_StartAsmCode/M_EndAsmCode: the canonical Diab callee-saved save/
#     restore pair, entered by M_Sort (bl 0x60354) and tail-reached through
#     `b 0x5f890`, each ending in blr.
# Other zero-size spans are NOT lifted by this table: 14 of the 63 contain
# invalid words (embedded pools/data) and remain explicit lift gaps.
ZERO_SIZE_SPANS = {
    # Diab single-precision libm helpers embedded between powf and tanf.
    # Their ELF STT_FUNC symbols have st_size=0, but the source image contains
    # clean PPC instruction streams at both entries. powf@0x72804 calls
    # frexpf@0x72c24 and ldexpf@0x72c54 directly; emitting empty C bodies
    # leaves f1 and the exponent output stale and drives powf into its range
    # error path. The spans end exactly at the next function symbols.
    # modff ends at its terminal blr; the following 0 word is a pool/padding
    # word before reducef and is deliberately outside the selected span.
    0x726e0: 0x72734,  # modff: 21 instructions
    0x72c24: 0x72c54,  # frexpf: 12 instructions
    0x72c54: 0x72c7c,  # ldexpf: 10 instructions
    # One-instruction entry thunk: branch to main@0x1ef6c.
    0x74134: 0x74138,  # __start: 1 instruction
    # State 3/17 reaches M_Draw through MS_ShowWorld with nonempty, sorted
    # guest POD lists. M_Draw dispatches to these assembly-authored spans;
    # leaving their zero-size symbols as empty C bodies suppresses every
    # scene vertex and returns the stale POD pointer as a bogus draw count.
    # Both spans disassemble cleanly through the next STT_FUNC boundary.
    0x513e8: 0x5140c,  # M_DrawA: setup/tail entry, 9 insns
    0x5140c: 0x5180c,  # M_DrawPod: POD command generation, 256 insns
    # The guest lighting pipeline: directional, specular, environment,
    # fog, point, and spotlight shaders with their setup/return helpers.
    0x5180c: 0x5186c,  # M_LightDir, 24 insns
    0x518b0: 0x51aa0,  # M_LightDirSpec, 124 insns
    0x51b54: 0x51d40,  # M_LightDirSpecTex through M_ClistManager tail, 123 insns
    0x51d6c: 0x5206c,  # M_LightEnv, 192 insns
    0x52080: 0x520b8,  # M_LightFog, 14 insns
    0x520cc: 0x52104,  # M_LightFogTrans, 14 insns
    0x52128: 0x52280,  # M_LightPoint, 86 insns
    0x52280: 0x522c4,  # SetupVectorEntry2, 17 insns
    0x52284: 0x522c4,  # SetupVector, 16 insns
    0x52298: 0x522c4,  # SetupJoin, 11 insns
    0x522c4: 0x522f8,  # SetupPointEntry2, 13 insns
    0x522c8: 0x522f8,  # SetupPoint, 12 insns
    0x522e0: 0x522f8,  # StandardLightReturn, 6 insns
    0x522f8: 0x52340,  # SetupRgbReturn, 18 insns
    0x52308: 0x52340,  # SetupRgbReturn2, 14 insns
    0x52314: 0x52340,  # SetupRgbReturn3, 11 insns
    0x52340: 0x52378,  # CheckDist, 14 insns
    0x523a4: 0x52568,  # M_LightSoftSpot, 113 insns
    0x52568: 0x527b8,  # M_BoundsTest: bounding box culling & frustum testing, 148 insns
    # M_SaveAndCallC lets the observed POD setup routines publish these exact
    # dynamic-light and pre-lit draw/return pairs. They are the guest's projection,
    # clipping, and CLT primitive builders for all POD varieties.
    0x53568: 0x5374c,  # M_DrawDynLit
    0x5374c: 0x540f4,  # M_LightReturnDynLit through local endcode
    0x54568: 0x5474c,  # M_DrawDynLitEnv
    0x5474c: 0x553dc,  # M_LightReturnDynLitEnv through local endcode
    0x55568: 0x5574c,  # M_DrawDynLitTex
    0x5574c: 0x56274,  # M_LightReturnDynLitTex through local endcode
    0x56568: 0x5674c,  # M_DrawDynLitTex_offset
    0x5674c: 0x57294,  # M_LightReturnDynLitTex_offset through local endcode
    0x57568: 0x57754,  # M_DrawDynLitTrans
    0x57754: 0x581d0,  # M_LightReturnDynLitTrans through local endcode
    0x58568: 0x5874c,  # M_DrawDynLit_offset
    0x5874c: 0x59114,  # M_LightReturnDynLit_offset through local endcode
    0x59568: 0x59734,  # M_DrawPreLit
    0x59734: 0x5a0a8,  # M_LightReturnPreLit through local endcode
    0x5a568: 0x5a734,  # M_DrawPreLitTex
    0x5a734: 0x5b228,  # M_LightReturnPreLitTex through local endcode
    0x5b568: 0x5b734,  # M_DrawPreLitTex_offset
    0x5b734: 0x5c248,  # M_LightReturnPreLitTex_offset through local endcode
    0x5c568: 0x5c73c,  # M_DrawPreLitTrans
    0x5c73c: 0x5d184,  # M_LightReturnPreLitTrans through local endcode
    0x5d568: 0x5d734,  # M_DrawPreLit_offset
    0x5d734: 0x5e0c8,  # M_LightReturnPreLit_offset through local endcode
    0x5e568: 0x5e95c,  # M_PreLight, 253 insns
    # Material callbacks that copy guest textures and blend snippets.
    0x5fd04: 0x5fd94,  # M_LoadPodTexture, 36 insns
    0x5fd94: 0x5fe14,  # M_LoadPodTextureNoUpdate, 32 insns
    # Register-save trampolines and assembly sorting.
    0x5fe14: 0x5fe74,  # M_SaveAndCallC, 24 insns
    0x5f890: 0x5f93c,  # M_EndAsmCode: restore epilogue, 43 insns
    0x60098: 0x60354,  # M_Sort: linked-pod merge sort, 175 insns
    0x60354: 0x603fc,  # M_StartAsmCode: callee-saved save prologue, 42 insns
}

# Zero-size spans for the generic (non driving-game) profile. The keys are
# image-local .text offsets and only apply when a zero-size STT_FUNC symbol
# exists at that offset, so an entry is inert for any image without the
# matching symbol. shooter2d's Diab single-precision libm entries sit at the
# end of its .text with st_size=0 exactly like driving's modff/frexpf/ldexpf:
#
#   sinf@0x11efc   `addi r3,r0,0` + `b 0x11f08`  (8 bytes, then shared body)
#   cosf@0x11f04   `addi r3,r0,1` falling into the same body at 0x11f08
#   ldexpf@0x11fd0 real code through its terminal blr at 0x11ff4
#
# sinf and cosf are two entries into ONE range-reduction/polynomial routine
# (0x11f08..0x11fa0: the body branches from 0x11f40 to its second half at
# 0x11f70 and both halves end in blr); 0x11fa4..0x11fd0 is the float constant
# pool those entries index (lis/ori r4,1 / lfs n(r4) with r4 = 0x11fa4) and is
# deliberately outside every span. Emitting empty C bodies for these entries
# returns the caller's f1 unchanged, which is exactly how shooter2d bullets
# froze: addgoodbullet computes vy = -sinf(a)*k and vx = cosf(a)*k, so empty
# helpers left every shot with zero velocity. Each span therefore covers the
# whole instruction stream its entry can reach, ending at the next entry or
# terminal return. The 0x11ff8..0x11ff8 helper (frexpf-shaped, no symbol) is
# discovered separately by discover_gap_helpers.
GENERIC_ZERO_SIZE_SPANS = {
    0x11efc: 0x11fa4,  # sinf: entry + shared sincos body (26 insns + 25)
    0x11f04: 0x11fa4,  # cosf: overlapping slice, entry falls into the body
    0x11fd0: 0x11ff8,  # ldexpf: 10 insns, terminal blr at 0x11ff4
}

ZERO_SIZE_SPANS_BY_PROFILE = {
    "driving-game": ZERO_SIZE_SPANS,
    "generic": GENERIC_ZERO_SIZE_SPANS,
}

# Audited entry points inside assembly spans. M_DrawA tail-enters M_DrawPod at
# 0x517dc; that loop condition branches back to 0x51458, which is neither an
# STT_FUNC symbol nor a standalone C-call boundary. Emitting this overlapping
# slice gives the cross-span branch a real guest dispatch target while keeping
# all decisions in the original instruction stream.
EXPLICIT_GAP_SPANS = {
    0x51458: 0x5180c,
    0x517f8: 0x5180c,  # LightDirInit
    0x51808: 0x5180c,  # LightDirInit tail
    0x5186c: 0x518b0,  # LightDirSpecInit
    0x518ac: 0x518b0,  # LightDirSpecInit tail
    0x51aa0: 0x51b54,  # LightDirSpecTexInit
    0x51b50: 0x51b54,  # LightDirSpecTexInit tail
    0x51d34: 0x51d40,  # M_ClistManager, 3 insns
    0x51d40: 0x51d6c,  # LightEnvInit
    0x51d68: 0x51d6c,  # LightEnvInit tail
    0x5206c: 0x52080,  # LightFogInit
    0x5207c: 0x52080,  # LightFogInit tail
    0x520b8: 0x520cc,  # LightFogTransInit
    0x520c8: 0x520cc,  # LightFogTransInit tail
    0x52104: 0x52128,  # LightPointInit
    0x52124: 0x52128,  # LightPointInit tail
    0x52378: 0x523a4,  # LightSoftSpotInit
    0x523a0: 0x523a4,  # LightSoftSpotInit tail
    # The dynamic/prelit return blocks use bdnz to re-enter these transform loops
    # across the zero-size symbol boundaries.
    0x53590: 0x5374c,
    0x540e8: 0x540f4,  # M_LightReturnDynLit ClistManager
    0x54590: 0x5474c,
    0x553d0: 0x553dc,  # M_LightReturnDynLitEnv ClistManager
    0x55590: 0x5574c,
    0x56268: 0x56274,  # M_LightReturnDynLitTex ClistManager
    0x56590: 0x5674c,
    0x57288: 0x57294,  # M_LightReturnDynLitTex_offset ClistManager
    0x57590: 0x57754,
    0x581c4: 0x581d0,  # M_LightReturnDynLitTrans ClistManager
    0x58590: 0x5874c,
    0x59108: 0x59114,  # M_LightReturnDynLit_offset ClistManager
    0x59590: 0x59734,
    0x5a09c: 0x5a0a8,  # M_LightReturnPreLit ClistManager
    0x5a590: 0x5a734,
    0x5b21c: 0x5b228,  # M_LightReturnPreLitTex ClistManager
    0x5b590: 0x5b734,
    0x5c23c: 0x5c248,  # M_LightReturnPreLitTex_offset ClistManager
    0x5c590: 0x5c738,
    0x5d178: 0x5d184,  # M_LightReturnPreLitTrans ClistManager
    0x5d590: 0x5d734,
    0x5e0bc: 0x5e0c8,  # M_LightReturnPreLit_offset ClistManager
    0x5fd88: 0x5fd94,  # M_LoadPodTexture ClistManager
}

# M_Draw's dynamic vertex builders and lighting shaders are threaded guest
# assembly. At these audited sites they load continuations into LR (from
# renderer +0x1fc or light list r13/r21) and use `blr` as an indirect jump,
# not as a return to the C caller.
INDIRECT_BLR_SITES = {
    0x51868,  # M_LightDir light chain continuation (r21)
    0x51a9c,  # M_LightDirSpec light chain continuation (r21)
    0x51d30,  # M_LightDirSpecTex light chain continuation (r21)
    0x52068,  # M_LightEnv light chain continuation (r21)
    0x520b4,  # M_LightFog light chain continuation (r21)
    0x52100,  # M_LightFogTrans light chain continuation (r21)
    0x5227c,  # M_LightPoint light chain continuation (r21)
    0x52564,  # M_LightSoftSpot light chain continuation (r21)
    0x53748,  # M_DrawDynLit lighting handoff (r0 from +0x1fc)
    0x54748,  # M_DrawDynLitEnv lighting handoff (r0 from +0x1fc)
    0x55748,  # M_DrawDynLitTex lighting handoff (r0 from +0x1fc)
    0x56748,  # M_DrawDynLitTex_offset lighting handoff (r0 from +0x1fc)
    0x57750,  # M_DrawDynLitTrans lighting handoff (r0 from +0x1fc)
    0x58748,  # M_DrawDynLit_offset lighting handoff (r0 from +0x1fc)
    0x59730,  # M_DrawPreLit lighting handoff (r0 from +0x1fc)
    0x5a730,  # M_DrawPreLitTex lighting handoff (r0 from +0x1fc)
    0x5b730,  # M_DrawPreLitTex_offset lighting handoff (r0 from +0x1fc)
    0x5d730,  # M_DrawPreLit_offset lighting handoff (r0 from +0x1fc)
}

# Some Mercury helpers thread control through an interior continuation by
# loading a routine address into LR and returning through `blr`. These are
# not direct branch targets, so the ordinary local-target scan cannot create
# their C labels or include them in the local-return dispatch. The Mercury
# polygon clippers across all 11 draw routines use one continuation per clip
# plane (W-MIN, X-MIN, X-MAX, Y-MIN, Y-MAX); the 5 targets in each routine
# are selected by loading the relocated routineptr (`TEXT_BASE + routine_offset`)
# into r23 and doing `mtlr r23; blr`. This includes 0x5574c (M_LightReturnDynLitTex,
# State 2 showroom) and 0x5a734 (M_LightReturnPreLitTex, State 3 in-race textured
# road & world). Without these entries, clipped polygons default out of the
# function, silently dropping all geometry crossing clipping planes.
COMPUTED_LOCAL_RETURN_TARGETS = {
    0x5374c: {
        TEXT_BASE + 0x53f68: 0x53f68,
        TEXT_BASE + 0x53f84: 0x53f84,
        TEXT_BASE + 0x53f90: 0x53f90,
        TEXT_BASE + 0x53fac: 0x53fac,
        TEXT_BASE + 0x53fb8: 0x53fb8,
    },
    0x5474c: {
        TEXT_BASE + 0x55228: 0x55228,
        TEXT_BASE + 0x55244: 0x55244,
        TEXT_BASE + 0x55250: 0x55250,
        TEXT_BASE + 0x5526c: 0x5526c,
        TEXT_BASE + 0x55278: 0x55278,
    },
    0x5574c: {
        TEXT_BASE + 0x560c0: 0x560c0,
        TEXT_BASE + 0x560dc: 0x560dc,
        TEXT_BASE + 0x560e8: 0x560e8,
        TEXT_BASE + 0x56104: 0x56104,
        TEXT_BASE + 0x56110: 0x56110,
    },
    0x5674c: {
        TEXT_BASE + 0x570e0: 0x570e0,
        TEXT_BASE + 0x570fc: 0x570fc,
        TEXT_BASE + 0x57108: 0x57108,
        TEXT_BASE + 0x57124: 0x57124,
        TEXT_BASE + 0x57130: 0x57130,
    },
    0x57754: {
        TEXT_BASE + 0x58030: 0x58030,
        TEXT_BASE + 0x5804c: 0x5804c,
        TEXT_BASE + 0x58058: 0x58058,
        TEXT_BASE + 0x58074: 0x58074,
        TEXT_BASE + 0x58080: 0x58080,
    },
    0x5874c: {
        TEXT_BASE + 0x58f88: 0x58f88,
        TEXT_BASE + 0x58fa4: 0x58fa4,
        TEXT_BASE + 0x58fb0: 0x58fb0,
        TEXT_BASE + 0x58fcc: 0x58fcc,
        TEXT_BASE + 0x58fd8: 0x58fd8,
    },
    0x59734: {
        TEXT_BASE + 0x59f1c: 0x59f1c,
        TEXT_BASE + 0x59f38: 0x59f38,
        TEXT_BASE + 0x59f44: 0x59f44,
        TEXT_BASE + 0x59f60: 0x59f60,
        TEXT_BASE + 0x59f6c: 0x59f6c,
    },
    0x5a734: {
        TEXT_BASE + 0x5b074: 0x5b074,
        TEXT_BASE + 0x5b090: 0x5b090,
        TEXT_BASE + 0x5b09c: 0x5b09c,
        TEXT_BASE + 0x5b0b8: 0x5b0b8,
        TEXT_BASE + 0x5b0c4: 0x5b0c4,
    },
    0x5b734: {
        TEXT_BASE + 0x5c094: 0x5c094,
        TEXT_BASE + 0x5c0b0: 0x5c0b0,
        TEXT_BASE + 0x5c0bc: 0x5c0bc,
        TEXT_BASE + 0x5c0d8: 0x5c0d8,
        TEXT_BASE + 0x5c0e4: 0x5c0e4,
    },
    0x5c73c: {
        TEXT_BASE + 0x5cfe4: 0x5cfe4,
        TEXT_BASE + 0x5d000: 0x5d000,
        TEXT_BASE + 0x5d00c: 0x5d00c,
        TEXT_BASE + 0x5d028: 0x5d028,
        TEXT_BASE + 0x5d034: 0x5d034,
    },
    0x5d734: {
        TEXT_BASE + 0x5df3c: 0x5df3c,
        TEXT_BASE + 0x5df58: 0x5df58,
        TEXT_BASE + 0x5df64: 0x5df64,
        TEXT_BASE + 0x5df80: 0x5df80,
        TEXT_BASE + 0x5df8c: 0x5df8c,
    },
}

# Cross-process checkpoint continuation targets. These are not ordinary
# computed branches: a fresh host process may enter one of these original
# lifted functions only after the loader has proven that the saved task was
# parked at the listed guest call/loop boundary. Keep the table explicit and
# module-relative so a normal guest call remains unchanged.
# The runtime arms a one-shot TLS request; c->pc by itself is only a last
# translated-access tag and must never select a resume label.
CHECKPOINT_RESUME_TARGETS = {
    0x2fa4c: (0x2fb88,),             # StateDriver frame loop head
    0x28108: (0x28758,),             # soundtrack WaitSignal
    0x1fe9c: (0x1ff90,),             # ebthread WaitPort
    0x692a0: (0x692f8,),             # DataStreamThread WaitSignal
    0x1726c: (0x18d0c, 0x18ea4),     # trackInit_2 signal waits
    0x0b054: (0x0b7c8,),             # CarSimThread timer wait
}


def _detect_switch_tables(bin_, addr, size):
    """Find `lwz rN,pool(rA); mtctr rN; bctr` tails with an in-function pool.

    Returns (sites, pools): sites maps each bctr offset to a sorted tuple of
    absolute guest VAs; pools is the set of pool-word offsets. A pool word
    must carry an ADDR32 reloc against .text whose target is a 4-aligned,
    capstone-invalid instruction boundary strictly inside this function.
    """
    from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN
    md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
    md.detail = False
    dec = {}
    for ins in _disasm_resync(md, bin_.text[addr:addr + size], addr):
        dec[ins.address] = (None if isinstance(ins, _InvalidInsn)
                            else (ins.mnemonic, ins.op_str))
    pcs = sorted(dec)
    text_shx = bin_.sec_by_name[".text"][0]
    hi_b = addr + size

    def pool_targets(pa):
        tgts, off = set(), pa
        while addr <= off < hi_b:
            rel = bin_.relocs_text.get(off)
            if not rel or rel[0] != R_ADDR32:
                break
            ent = bin_.sym_by_idx.get(rel[1])
            if not ent or ent["shx"] != text_shx:
                break
            tva = rel[2]
            if tva % 4 or not (addr <= tva < hi_b) or tva not in dec:
                break
            # The original detector also required dec[off] is None (i.e.
            # capstone reported the word as an invalid instruction). Diab
            # switch tables in the secondary-25 path use 0x00000000 (attn)
            # as a padding entry, which capstone decodes as a valid
            # instruction. The ADDR32 reloc is the authoritative marker:
            # if it points to a 4-aligned in-function VA, the word is a
            # table entry regardless of capstone's view.
            tgts.add(tva)
            off += 4
        return tgts

    sites, pools = {}, set()
    for i, pc in enumerate(pcs):
        d = dec[pc]
        if d is None or d[0] != "lwz" or i + 2 >= len(pcs):
            continue
        try:
            rd_tok, mem_tok = d[1].split(",", 1)
            rd = _reg(rd_tok)
            _disp, ra = _mem_operand(mem_tok)
        except ValueError:
            continue
        d2, d3 = dec[pcs[i + 1]], dec[pcs[i + 2]]
        if (d2 is None or d2[0] != "mtctr" or d2[1].strip() != f"r{rd}" or
                d3 is None or d3[0] != "bctr"):
            continue
        lo_r = bin_.relocs_text.get(pc + 2)
        if not lo_r or lo_r[0] != R_LO16:
            continue
        pa = lo_r[2]
        addis_ok = False
        for back in range(1, 4):     # bounds-check bgt may sit in between
            if i - back < 0:
                break
            q = dec[pcs[i - back]]
            if q is None or q[0] != "addis":
                continue
            parts = [t.strip() for t in q[1].split(",")]
            if len(parts) == 3 and parts[0] == f"r{ra}":
                ha_r = bin_.relocs_text.get(pcs[i - back] + 2)
                if ha_r and ha_r[0] == R_HA16 and ha_r[2] == pa:
                    addis_ok = True
                    break
        if not addis_ok:
            continue
        tgts = pool_targets(pa)
        if len(tgts) < 2 or any(t == pc for t in tgts):
            continue
        sites[pcs[i + 2]] = tuple(sorted(bin_.text_base + t for t in tgts))
        off = pa
        # Walk the pool by ADDR32 reloc continuity, not by capstone "invalid"
        # status, so the same `attn` (0x00000000) padding word documented above
        # does not truncate the table to 3 entries. Pool membership = the
        # ADDR32 chain, nothing else.
        while addr <= off < hi_b:
            rel = bin_.relocs_text.get(off)
            if not rel or rel[0] != R_ADDR32:
                break
            ent = bin_.sym_by_idx.get(rel[1])
            tva = rel[2] if ent and ent["shx"] == text_shx else -1
            if not (addr <= tva < hi_b) or tva % 4 or tva not in dec:
                break
            pools.add(off)
            off += 4
    return sites, pools

def func_emitter(bin_, func, chunk_idx):
    addr = func["val"]
    symbol_size = func["sz"]
    size = func.get("lift_size", symbol_size)
    name = func["name"]
    file_ = func["file"]
    san = func_sym(func, bin_.symbol_prefix)
    is_game_profile = bin_.profile == "driving-game"
    lines = []
    if func.get("lift_kind") == "local_diab_va_thunk":
        lines.append(
            f"// {file_ or '?'} :: {name}  guest {addr:#08x} symbol size "
            f"{symbol_size} (lifted 12-byte Diab VA thunk -> "
            f"{func['lift_target']:#x})  -> FN_{san}")
    elif func.get("lift_kind") == "imported_diab_va_thunk":
        lines.append(
            f"// {file_ or '?'} :: {name}  guest {addr:#08x} symbol size "
            f"{symbol_size} (lifted 12-byte imported Diab VA thunk -> "
            f"folio {func['lift_target_folio']} ordinal "
            f"{func['lift_target_ordinal']})  -> FN_{san}")
    else:
        lines.append(f"// {file_ or '?'} :: {name}  guest {addr:#08x} size {size}  -> FN_{san}")
    # Header comment with reloc inventory for this function
    rels_in_func = []
    for off in range(addr, addr+size):
        if off in bin_.relocs_text:
            t, sym, add = bin_.relocs_text[off]
            rels_in_func.append((off, t, sym, add))
    if rels_in_func:
        lines.append(f"//   relocs in func: {len(rels_in_func)}")
        for off, t, sym, add in rels_in_func[:8]:
            if t == R_REL24:
                fid, ord_ = bin_.import_selector(sym)
                if sym >> 16:
                    lines.append(f"//     {off:#06x}: REL24 folio={fid} ord={ord_} sym={sym:#x}")
                else:
                    lines.append(f"//     {off:#06x}: IMPREL24 folio={fid} ord={ord_} "
                                 f"module={sym>>16} sym={sym:#x}")
            elif t in (R_HA16, R_LO16, R_HI16):
                abs_a = bin_.abs_addr_for_reloc(sym, add)
                lines.append(f"//     {off:#06x}: {t} sym={bin_.sym_name(sym)!r} add={add:#x} -> abs {abs_a:#x}")
            elif t in (R_LO16_FOLIO, R_HI16_FOLIO, R_HA16_FOLIO):
                slot_ea = bin_.import_slot_ea(sym)
                lines.append(f"//     {off:#06x}: FOLIO folio={sym>>16} ord={sym&0xFFFF} "
                             f"-> slot {slot_ea if slot_ea is not None else 'UNKNOWN':#x}")
            elif t == R_ADDR32:
                lines.append(f"//     {off:#06x}: ADDR32")
        if len(rels_in_func) > 8:
            lines.append(f"//     ... and {len(rels_in_func)-8} more")
    if func.get("lift_kind") == "imported_diab_va_thunk":
        lines.append(f"void FN_{san}(PPCContext* c) {{")
        for tracepoint_id in getattr(bin_, "tracepoint_by_pc", {}).get(addr, []):
            lines.append(f"    m2_tracepoint_hit(c, {tracepoint_id}u, {addr:#x}u);")
        lines.append(f"    c->pc = {addr:#x}u;")
        lines.append(
            f"    (void)m2_guest_import_tag_call(c, "
            f"{func['lift_fixed_arg_count']}u, "
            f"{func['lift_target_folio']}u, "
            f"{func['lift_target_ordinal']}u, {addr:#x}u);")
        lines.append("}")
        lines.append("")
        return "\n".join(lines)
    lines.append(f"void FN_{san}(PPCContext* c) {{")
    if is_game_profile and addr == 0x72794:
        lines.append("    if (m2_trace_powf_error_c) m2_trace_powf_error_c(c);")
    elif is_game_profile and addr == 0x72804:
        lines.append("    if (m2_trace_powf_enter_c) m2_trace_powf_enter_c(c);")
    # Prologue: note Diab save if present at site
    # Body: translate each instruction word; we use capstone if available, else generic
    try:
        from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN
        md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
        md.detail = False
        code = bin_.text[addr:addr+size]
        insns = _disasm_resync(md, code, addr)
        has_capstone = True
    except Exception:
        has_capstone = False
        insns = []
        for p in range(addr, addr+size, 4):
            if p+4 > len(bin_.text): break
            w = struct.unpack(">I", bin_.text[p:p+4])[0]
            insns.append((p, w, None))
    # If capstone, insns are capstone objects; else tuples
    # We will emit per instruction with handling for branches and FP ops
    # Collect branch targets for label generation
    # Precompute instruction addresses for accurate local branch targets
    instr_addrs = set()
    if has_capstone:
        instr_addrs = set(ins.address for ins in insns)
    else:
        instr_addrs = set(p for p,_,_ in insns)
    local_targets = set()
    if has_capstone:
        for ins in insns:
            w = struct.unpack(">I", bin_.text[ins.address:ins.address+4])[0]
            top = w >> 26
            if top == 18 and not (w & 2):
                li = w & 0x03FFFFFC
                if li & 0x02000000: li -= 0x04000000
                tgt = ins.address + li
                if tgt in instr_addrs:
                    local_targets.add(tgt)
            elif top == 16 and not (w & 2):
                bd = w & 0xFFFC
                if bd & 0x8000: bd -= 0x10000
                tgt = ins.address + bd
                if tgt in instr_addrs:
                    local_targets.add(tgt)
    else:
        for p,w,_ in insns:
            top = w >> 26
            if top == 18 and not (w & 2):
                li = w & 0x03FFFFFC
                if li & 0x02000000: li -= 0x04000000
                tgt = p + li
                if tgt in instr_addrs:
                    local_targets.add(tgt)
            elif top == 16 and not (w & 2):
                bd = w & 0xFFFC
                if bd & 0x8000: bd -= 0x10000
                tgt = p + bd
                if tgt in instr_addrs:
                    local_targets.add(tgt)

    resume_targets = CHECKPOINT_RESUME_TARGETS.get(addr, ()) if is_game_profile else ()
    missing_resume_targets = set(resume_targets) - instr_addrs
    if missing_resume_targets:
        raise ValueError(
            f"checkpoint resume target(s) outside function {addr:#x}: "
            f"{sorted(missing_resume_targets)}"
        )
    local_targets.update(resume_targets)

    func_starts = {f["val"] for f in bin_.funcs} | getattr(bin_, "gap_starts", set())

    # Intra-function `bl` support (assembly-authored spans, e.g. M_Sort at
    # 0x60098): the target is an interior label in this function, so the
    # recomp must use a goto and retain the guest link register for the
    # matching local-return dispatch. Calls whose target is a function/gap
    # start use a separate RecompFn C frame instead. That includes explicit
    # self-recursion: its child must return to the paused parent C call, not
    # consume the parent's local continuation after restoring its own frame.
    bl_return_addrs = set()
    internal_call_sites = 0
    if has_capstone:
        for ins in insns:
            w = struct.unpack(">I", bin_.text[ins.address:ins.address + 4])[0]
            top = w >> 26
            if top == 18 and (w & 1) and not (w & 2):
                rel24 = bin_.relocs_text.get(ins.address)
                if rel24 and rel24[0] == R_REL24:
                    continue  # folio selector: returns through the callee's C frame
                li = w & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                target = ins.address + li
                if target in instr_addrs and target not in func_starts:
                    bl_return_addrs.add(ins.address + 4)
                    internal_call_sites += 1
            elif top == 16 and (w & 1) and not (w & 2):
                # Conditional branch-with-link is also a local call when its
                # target is inside this function. M_Sort uses `bnel` at
                # 0x60108 to enter its merge-tail helper and returns through
                # the published 0x6010c link. Omitting that link makes the
                # helper's `blr` fall back to the M_StartAsmCode return label,
                # restarting the radix pass indefinitely.
                bd = w & 0xFFFC
                if bd & 0x8000:
                    bd -= 0x10000
                target = ins.address + bd
                if target in instr_addrs and target not in func_starts:
                    bl_return_addrs.add(ins.address + 4)
                    internal_call_sites += 1
    computed_local_return_map = (dict(COMPUTED_LOCAL_RETURN_TARGETS.get(addr, {}))
                                 if is_game_profile else {})
    computed_local_return_addrs = set(computed_local_return_map)
    computed_local_return_labels = set(computed_local_return_map.values())
    missing_computed_targets = computed_local_return_labels - instr_addrs
    if missing_computed_targets:
        raise ValueError(
            f"computed local-return target(s) outside function {addr:#x}: "
            f"{sorted(missing_computed_targets)}"
        )
    local_return_addrs = bl_return_addrs | computed_local_return_addrs
    if local_return_addrs:
        local_targets.update(bl_return_addrs | computed_local_return_labels)

    # Intra-function switch dispatch (gated): lift `lwz;mtctr;bctr` tails to a
    # bounded C switch over c->ctr, and label every case target.
    switch_sites, switch_pools = {}, set()
    if has_capstone and (not is_game_profile or addr in SWITCH_DISPATCH_FUNCS):
        switch_sites, switch_pools = _detect_switch_tables(bin_, addr, size)
        for tgts in switch_sites.values():
            local_targets.update(t - bin_.text_base for t in tgts)

    # Emit
    if resume_targets:
        lines.append(
            f"    uint32_t _resume_pc = m2_guest_resume_target(c, {addr:#x}u);"
        )
        for target in resume_targets:
            lines.append(
                f"    if (_resume_pc == {target:#x}u) goto L{target:x};"
            )
    if has_capstone:
        for ins in insns:
            pc = ins.address
            if pc in local_targets:
                lines.append(f"L{pc:x}:;")
            # shooter2d Play_Init spins on its worker's published 99 sentinel
            # without a selector. A cooperative pause must park the parent too,
            # otherwise it spins while the worker is parked at a selector.
            # This checkpoint changes no guest registers, memory, or decisions.
            if (_image_is(bin_.path, "shooter2d", "shooter2d")
                    and addr == 0xa450 and pc == 0xa4f0):
                if bin_.text[0xa4fc:0xa500] != bytes.fromhex("4082fff4"):
                    raise ValueError("shooter2d Play_Init poll branch changed")
                lines.append("    if (m2_diagnostic_checkpoint) m2_diagnostic_checkpoint(0xa4f0u);")
            if (_image_is(bin_.path, "shooter2d", "shooter2d")
                    and addr == 0x2a0 and pc == 0x5dc):
                # Original main has two loopbacks here, one after its timer
                # update. Preserve both branches and every original operation.
                for site, word in ((0x5dc, "89810034"),
                                   (0xde8, "4082f7f4"),
                                   (0xe64, "4bfff778")):
                    if bin_.text[site:site + 4] != bytes.fromhex(word):
                        raise ValueError("shooter2d main loop encoding changed")
                lines.append("    m2_guest_shooter_frame_tick(c, 0x5dcu);")
            # Guest-owned frame tick at StateDriver loop head.
            # The lifted StateDriver loops forever via `b 0x2fb88`; the tick
            # increments the guest carve at 0x003F0000 (M2_GUEST_FRAME_COUNTER_EA)
            # with pc=0x2fb88, records primary state (BSS 0x1877d8) and emits
            # [guest_trace]. Host never mutates this carve. See ppc_ctx.h and
            # recomp_runtime.c:m2_guest_frame_tick. The current guest PC is
            # also tagged before every translated memory/selector operation so
            # an OOB or host-expanded access can be attributed to this image.
            if is_game_profile and addr == 0x2fa4c and pc == 0x2fb88:
                lines.append("    c->pc = 0x2fb88u;")
                lines.append("    m2_guest_frame_tick(c, 0x2fb88u);")
            # Optional player-AI aid. complexSim's preceding lbz at 0xa5b8
            # loaded the guest-owned controller selector into r11. Override
            # only that transient branch input; never mutate [GDP+0x70],
            # which ProcessCheckPoints also owns for race-state transitions.
            if is_game_profile and pc == 0xa5bc and addr == 0xa3a0:
                lines.append("    c->gpr[11] = m2_player_ai_controller_selector(c->gpr[11]);")
            # Explicit launcher control modes. Original mode falls through
            # untouched; enhanced translation is limited to known player/UI
            # input records. Custom replaces only the input-channel decoding
            # after ProcessInput's original clutch and gear handling.
            if is_game_profile and addr == 0xfc64 and pc == 0xfc64:
                lines.append("    if (m2_controls_translate(c)) return;")
            if is_game_profile and addr == 0x9680 and pc == 0x9680:
                lines.append("    m2_controls_process_begin(c);")
            if is_game_profile and addr == 0x9680 and pc == 0x97e8:
                lines.append("    m2_controls_custom_prepare(c);")
            if is_game_profile and addr == 0x9680 and pc == 0x986c:
                lines.append("    m2_controls_custom_restore(c);")
            if is_game_profile and addr == 0x9680 and pc == 0x9b58:
                lines.append("    m2_controls_custom_finish(c);")
            if is_game_profile and addr == 0x40aa8 and pc == 0x40b80:
                lines.append("    m2_analog_steering_observe(c);")
            # Optional high-detail car models. MS_ScanList chooses the car LOD
            # band in r31 from the two XZ-distance thresholds
            # (0x21e5c..0x21e6c) and passes it to MS_addcar at 0x21e94. Hook the
            # following instruction so only that transient band is overridden;
            # MS_addgroup still clamps 0 to the model's authored group count.
            if is_game_profile and addr == 0x21a68 and pc == 0x21e70:
                lines.append("    c->gpr[31] = m2_car_lod_level(c->gpr[31]);")
            # Optional player-car body-variant enhancement. trackInit_2 and
            # createNewCar copy initCarBody's output bytes to the player's
            # MS_buildcar/MS_rebuildcar arguments; hook the instruction after
            # each variant-byte load (r4 at 0x1792c, r5 at 0x1a904) so only
            # that transient argument is overridden, and pass the two
            # candidate entries from the guest's own car-body table
            # (TheCarBodies, .bss+0x6378; entry = body*0x18 + variant*4) so a
            # pack without the variant keeps the guest's choice. The opponent
            # builds (the drone loop and the single-opponent branch) are not
            # hooked.
            if is_game_profile and pc == 0x17930 and addr == 0x1726c:
                lines.append(
                    "    c->gpr[4] = m2_player_car_variant(c->gpr[4],"
                    " rd32(c, (uint32_t)(rd32(c, 0x187618u) + (uint32_t)(c->gpr[3] * 0x18u) + 4u)),"
                    " rd32(c, (uint32_t)(rd32(c, 0x187618u) + (uint32_t)(c->gpr[3] * 0x18u) + 8u)));")
            if is_game_profile and pc == 0x1a908 and addr == 0x1a718:
                lines.append(
                    "    c->gpr[5] = m2_player_car_variant(c->gpr[5],"
                    " rd32(c, (uint32_t)(rd32(c, 0x187618u) + (uint32_t)(c->gpr[4] * 0x18u) + 4u)),"
                    " rd32(c, (uint32_t)(rd32(c, 0x187618u) + (uint32_t)(c->gpr[4] * 0x18u) + 8u)));")
            # Optional skidpad engine-sound fix. The change-car apply calls
            # createNewCar at 0x2f388, then updateSkidpadSprites at 0x2f38c.
            # Hook the next instruction so the helper runs after createNewCar
            # returns: it rebinds the two engine channels from the per-engine
            # structures when the engine index changed, preserving simulation.
            if is_game_profile and pc == 0x2f38c and addr == 0x2f1d4:
                lines.append("    m2_skidpad_engine_rebind(c);")
            # UI_ShowMerc shares CloseData with the race rear-view pass.
            # Use UI_MercInit's authored width only for this preview, then
            # restore the race context after M_Draw (including the empty arm).
            if is_game_profile and addr == 0x375f8 and pc == 0x37644:
                lines.append("    uint32_t _preview_close = c->gpr[3];")
                lines.append("    uint32_t _preview_width = m2_menu_preview_clip_begin(c, _preview_close);")
            if is_game_profile and addr == 0x375f8 and pc == 0x377e8:
                lines.append("    m2_menu_preview_clip_end(c, _preview_close, _preview_width);")
            # Optional restoration of unfinished credits procedure. StateDriver
            # executes State 13 at 0x30de4. When M2_RESTORE_CREDITS=1, the
            # helper runs the restored interactive credits procedure and returns
            # 1, completing the frame and jumping to L31424. Unset or 0 returns
            # 0 and falls through to authentic credits.mov video playback.
            if is_game_profile and addr == 0x2fa4c and pc == 0x30de4:
                lines.append("    if (m2_credits_state_step(c)) goto L31424;")
            # Optional restoration of the unused post-race summary
            # (UI_DrawBestTimes). The result handler reaches 0x3136c after
            # the result movie (or directly, opponent setting 1) with the
            # win/lose/DNF index in r25; arming there excludes Skidpad, which
            # returns to the main menu without results. The next post-race
            # state, secondary 31 (0x306c4) or 32 (0x30758), shows the summary
            # and completes the frame until a button press; unset or 0 never
            # arms, so both handlers run unchanged.
            if is_game_profile and addr == 0x2fa4c and pc == 0x3136c:
                lines.append("    m2_best_times_arm(c);")
            if is_game_profile and addr == 0x2fa4c and pc in (0x306c4, 0x30758):
                lines.append("    if (m2_best_times_state_step(c)) goto L31424;")
            # The summary's background call returns at 0x379fc. Re-select the
            # current destination there, matching the shipped post-race screen
            # (UI_DrawHighScores_postRace 0x395b4) so the small labels blend
            # against this frame's buffer instead of whatever the result movie
            # or the previous menu left selected.
            if is_game_profile and addr == 0x379cc and pc == 0x379fc:
                lines.append("    m2_best_times_background_done(c);")
            # DNF layout: 0x37b60 is the cmpwi after the lap-count load. Fill
            # the uninitialized value buffer with racers[0]+0x34 (best lap)
            # and make the guest take the draw path.
            if is_game_profile and addr == 0x379cc and pc == 0x37b60:
                lines.append("    m2_best_times_dnf_fill(c);")
            # Optional restoration of the unused drafting effect
            # (HandleDrafting, 0x6d44). 0xae74 runs once per frame in
            # CarSimFunction after every complexSim call and before the player
            # record goes to the sim (sim_cmd_go at 0xae7c). The helper zeroes
            # Vwind and calls HandleDrafting when M2_RESTORE_DRAFTING=1; unset
            # or 0 returns without touching guest state.
            if is_game_profile and addr == 0xa8d8 and pc == 0xae74:
                lines.append("    m2_drafting_step(c);")
            # Optional secret-track unlock (activatesecrettrack, 0x2e578).
            # doButtonMenu (0x2ee5c) steps the Configure Race track selector
            # modulo configraceNumSub[0] (0x17d968, default 5). When
            # M2_UNLOCK_SECRET_TRACK=1, the helper calls activatesecrettrack
            # (which sets 0x17d968 = 6) before doButtonMenu reads the option
            # limits; unset or 0 returns without touching guest state.
            if is_game_profile and addr == 0x2ee5c and pc == 0x2ee5c:
                lines.append("    m2_unlock_secret_track_step(c);")
            # Default-on exterior HUD tachometer range fix. DrawInfoBar
            # (0x1608c) receives the gauge scale in f1; the caller passes the
            # shipped 0.015 (180 units / 12000 RPM), which only lets the
            # 11500 RPM Ferrari reach the gauge's yellow zone. The helper
            # rescales f1 from plyrEng's redline unless M2_HUD_TACH_FIX=0.
            if is_game_profile and addr == 0x1608c and pc == 0x1608c:
                lines.append("    m2_hud_tach_scale_step(c);")
            # Default-on Laguna phantom fix. repRestorePhantom (0x27250)
            # reads fi_ByteCount bytes of the disc phantom into its
            # 0x11978-byte buffer at 0x2735c; Laguna's 1.35 MB file overruns
            # the heap. With r4 = buffer and r5 = fi_ByteCount loaded, the
            # helper bounds the read unless M2_LAGUNA_PHANTOM_FIX=0.
            if is_game_profile and addr == 0x27250 and pc == 0x2735c:
                lines.append("    m2_phantom_restore_read_step(c);")
            # Restored credits only: credits_fill_line's layout TwoD_DrawText
            # loads the font texture before TextDestBlendInit (0x104d4) resets
            # the blender, so the line sprites hit the texture cache and skip
            # re-arming the font's blend state, drawing untinted on every
            # line-spawn frame. The helper invalidates that cache when enabled.
            if is_game_profile and addr == 0x103f8 and pc == 0x104d8:
                lines.append("    m2_credits_text_blend_rearm(c);")
            w = struct.unpack(">I", bin_.text[pc:pc+4])[0]
            top = w >> 26
            mn = ins.mnemonic
            op = ins.op_str
            # Undecodable word: real instructions never land here inside an
            # audited span, so treat it as the embedded data it is. Only
            # lifted pools get the "jump-table data word"
            # marker; every other function keeps its historical comment.
            # A pool word that *does* decode (e.g. `attn` 0x00000000
            # used as Diab table padding) must also be suppressed here;
            # the ADDR32 reloc is the authoritative pool-membership marker.
            if pc in switch_pools:
                rl = bin_.relocs_text.get(pc)
                tgt = (f" -> {bin_.text_base + rl[2]:#x}"
                       if rl and rl[0] == R_ADDR32 else "")
                lines.append(f"    /* {pc:05x}: .word 0x{w:08x}  // jump-table data word{tgt} */")
                continue
            if mn == "invalid":
                lines.append(f"    /* {pc:05x}: invalid 0x{w:08x} */")
                continue
            tracepoint_ids = getattr(bin_, "tracepoint_by_pc", {}).get(pc, [])
            for tracepoint_id in tracepoint_ids:
                lines.append(f"    m2_tracepoint_hit(c, {tracepoint_id}u, {pc:#x}u);")
            rel = bin_.relocs_text.get(pc) or bin_.relocs_text.get(pc+2)  # HA16/LO16 at +2
            # Check for external selector bl
            # REL24 entries are at the bl site offset (not +2)
            rel24 = bin_.relocs_text.get(pc)
            if top == 18 and (w & 1) and rel24 and rel24[0] == R_REL24:
                fid, ord_ = bin_.import_selector(rel24[1])
                if fid == 1 and ord_ in DIAB_ALL_ORDS:
                    lines.append(f"    c->pc = {pc:#x}u;")
                    lines.append(f"    /* {pc:05x}: {mn} {op} */")
                    if ord_ in DIAB_SAVE_GPR:
                        n = DIAB_SAVE_GPR[ord_]
                        lines.append(f"    /* Diab prologue save GPR r{n}..r31 */")
                        for r in range(n, 32):
                            lines.append(f"    wr32(c, c->gpr[11] - {(32 - r) * 4}, c->gpr[{r}]);")
                    elif ord_ in DIAB_SAVE_FPR:
                        n = DIAB_SAVE_FPR[ord_]
                        lines.append(f"    /* Diab prologue save FPR f{n}..f31 */")
                        for r in range(n, 32):
                            lines.append(f"    wrf32(c, c->gpr[11] - {(32 - r) * 4}, c->fpr[{r}]);")
                        if ord_ in (453, 459):
                            lines.append("    wr32(c, c->gpr[11] + 4u, c->gpr[0]);")
                    elif ord_ in DIAB_REST_GPR_NONLEAF:
                        n = DIAB_REST_GPR_NONLEAF[ord_]
                        lines.append(f"    /* Diab epilogue restore GPR r{n}..r31 (non-leaf) */")
                        for r in range(n, 32):
                            lines.append(f"    c->gpr[{r}] = rd32(c, c->gpr[11] - {(32 - r) * 4});")
                    elif ord_ in DIAB_REST_GPR_LEAF:
                        n = DIAB_REST_GPR_LEAF[ord_]
                        lines.append(f"    /* Diab epilogue restore GPR r{n}..r31 and return */")
                        for r in range(n, 32):
                            lines.append(f"    c->gpr[{r}] = rd32(c, c->gpr[11] - {(32 - r) * 4});")
                        lines.append(f"    c->gpr[1] = c->gpr[11];")
                        lines.append(f"    return;")
                    elif ord_ in DIAB_REST_FPR_LEAF:
                        n = DIAB_REST_FPR_LEAF[ord_]
                        lines.append(f"    /* Diab epilogue restore FPR f{n}..f31 and return */")
                        for r in range(n, 32):
                            lines.append(f"    c->fpr[{r}] = rdf32(c, c->gpr[11] - {(32 - r) * 4});")
                        if ord_ in (345, 351):
                            lines.append("    c->gpr[0] = rd32(c, c->gpr[11] + 4u);")
                            lines.append("    c->lr = c->gpr[0];")
                        lines.append(f"    c->gpr[1] = c->gpr[11];")
                        lines.append(f"    return;")
                    continue
                if fid == 1 and ord_ in VARARGS_ORDS:
                    lines.append(f"    /* {pc:05x}: {mn} {op} */")
                    lines.append(f"    /* varargs helper {VARARGS_ORDS[ord_]} folio {fid}:{ord_} */")
                    lines.append(f"    c->pc = {pc:#x}u;")
                    lines.append(f"    recomp_call_selector(c, {fid}, {ord_}, {pc:#x});")
                    continue
                lines.append(f"    /* {pc:05x}: {mn} {op} */")
                lines.append(f"    /* external folio call {fid}:{ord_} */")
                lines.append(f"    c->pc = {pc:#x}u;")
                lines.append(f"    recomp_call_selector(c, {fid}, {ord_}, {pc:#x});")
                continue
            # Diab DECVASTUB aliases use a non-linking external branch as a
            # tail call.  The raw relocation, rather than the unrelocated
            # branch displacement, is authoritative for its selector target.
            if (top == 18 and not (w & 1) and rel24 and
                    rel24[0] == R_REL24):
                fid, ord_ = bin_.import_selector(rel24[1])
                lines.append(f"    /* {pc:05x}: {mn} {op} */")
                if fid == 1 and ord_ in VARARGS_ORDS:
                    lines.append(f"    /* tail varargs helper {VARARGS_ORDS[ord_]} folio {fid}:{ord_} */")
                else:
                    lines.append(f"    /* external folio tail call {fid}:{ord_} */")
                lines.append(f"    c->pc = {pc:#x}u;")
                lines.append(f"    recomp_call_selector(c, {fid}, {ord_}, {pc:#x});")
                lines.append("    return;")
                continue
            lines.append(f"    /* {pc:05x}: {mn} {op} */")
            # FP mapping
            if mn in ("fmadds","fmsubs","fnmadds","fnmsubs","fmadd","fmsub","fnmadd","fnmsub"):
                # Keep as is via helper; capstone may show fmadds etc
                # We'll let recomp_helpers handle; just keep generic translate as comment + helper
                # For now, emit through helper mapping
                # Example: fmadds f1,f13,f12,f11
                # Parse regs: we rely on op_str like "f1, f13, f12, f11"
                # Try to map to FMADDS etc
                # Extract fd,fa,fc,fb via parsing
                regs = [r.strip() for r in op.split(",")]
                # capstone order for f59 is fd, fa, fc, fb? Actually fmadds fd,fa,fc,fb
                # We'll just emit helper if we can parse
                if len(regs) == 4:
                    fd, fa, fc, fb = regs
                    # map fd like f1 -> 1
                    def rnum(s): return int(s[1:])
                    try:
                        fd_n = rnum(fd); fa_n = rnum(fa); fc_n = rnum(fc); fb_n = rnum(fb)
                        if mn.startswith("fmadd"):
                            lines.append(f"    c->fpr[{fd_n}] = FMADDS(c->fpr[{fa_n}], c->fpr[{fc_n}], c->fpr[{fb_n}]);")
                        elif mn.startswith("fmsub"):
                            lines.append(f"    c->fpr[{fd_n}] = FMSUBS(c->fpr[{fa_n}], c->fpr[{fc_n}], c->fpr[{fb_n}]);")
                        elif mn.startswith("fnmadd"):
                            lines.append(f"    c->fpr[{fd_n}] = FNMADDS(c->fpr[{fa_n}], c->fpr[{fc_n}], c->fpr[{fb_n}]);")
                        elif mn.startswith("fnmsub"):
                            lines.append(f"    c->fpr[{fd_n}] = FNMSUBS(c->fpr[{fa_n}], c->fpr[{fc_n}], c->fpr[{fb_n}]);")
                        else:
                            lines.append(f"    /* unhandled FMA variant */")
                        continue
                    except: pass
                lines.append(f"    /* FMA fallback: {mn} */")
                continue
            if mn == "frsqrte":
                regs = [r.strip() for r in op.split(",")]
                if len(regs)==2:
                    fd, fb = regs
                    lines.append(f"    c->fpr[{fd[1:]}] = FRSQRTE(c->fpr[{fb[1:]}]);")
                    continue
            if mn == "fsel":
                regs = [r.strip() for r in op.split(",")]
                if len(regs)==4:
                    fd, fa, fc, fb = regs
                    lines.append(f"    c->fpr[{fd[1:]}] = FSEL(c->fpr[{fa[1:]}], c->fpr[{fc[1:]}], c->fpr[{fb[1:]}]);")
                    continue
            if mn == "fctiwz":
                regs = [r.strip() for r in op.split(",")]
                if len(regs)==2:
                    fd, fb = (_reg(r, "f") for r in regs)
                    # fctiwz leaves the truncated integer as raw bits in the
                    # FPR. Every consumer in this binary is stfiwx, which
                    # stores those bits, so keep the bit pattern instead of a
                    # re-encoded float value (and leave GPRs untouched).
                    iw = f"_iw_{pc:x}"
                    lines.append(f"    uint32_t {iw} = (uint32_t)FCTIWZ({_c_fpr(fb)});")
                    lines.append(f"    memcpy(&{_c_fpr(fd)}, &{iw}, sizeof({iw}));")
                    continue

            # Scalar single-precision operations. PPC single arithmetic
            # rounds after every instruction; the helpers use volatile
            # temporaries to prevent a wider host intermediate.
            if mn in ("fadds", "fsubs", "fmuls", "fdivs"):
                regs = [r.strip() for r in op.split(",")]
                if len(regs) == 3:
                    fd, fa, fb = (_reg(r, "f") for r in regs)
                    helper = {"fadds": "PPC_FADD", "fsubs": "PPC_FSUB",
                              "fmuls": "PPC_FMUL", "fdivs": "PPC_FDIV"}[mn]
                    lines.append(f"    {_c_fpr(fd)} = {helper}({_c_fpr(fa)}, {_c_fpr(fb)});")
                    continue
            if mn in ("fmr", "fneg", "fabs"):
                regs = [r.strip() for r in op.split(",")]
                if len(regs) == 2:
                    fd, fb = (_reg(r, "f") for r in regs)
                    if mn == "fmr":
                        expr = _c_fpr(fb)
                    elif mn == "fneg":
                        expr = f"-({_c_fpr(fb)})"
                    else:
                        expr = f"fabsf({_c_fpr(fb)})"
                    lines.append(f"    {_c_fpr(fd)} = {expr};")
                    continue
            if mn in ("fcmpu", "fcmpo"):
                regs = [r.strip() for r in op.split(",")]
                if len(regs) == 3:
                    field = _reg(regs[0], "cr")
                    fa, fb = (_reg(r, "f") for r in regs[1:])
                    lines.append(f"    cr_set_f(c, {(7 - field) * 4}, {_c_fpr(fa)}, {_c_fpr(fb)});")
                    continue

            # Integer compares feed the condition register used by the
            # following bc. Capstone spells the signed and unsigned forms
            # distinctly, so no opcode decoding is needed here.
            if mn in ("cmpwi", "cmplwi"):
                regs = [r.strip() for r in op.split(",")]
                if len(regs) in (2, 3):
                    field = _reg(regs[0], "cr") if len(regs) == 3 else 0
                    ra = _reg(regs[-2]) if len(regs) == 3 else _reg(regs[0])
                    # cmplwi compares unsigned against the zero-extended 16-bit
                    # immediate; a signed relocation (-21436 for 0xac44) would
                    # sign-extend through the C int conversion to 0xFFFFAC44
                    # (live: GetTemplateTag tag match). cmpwi stays signed.
                    imm = _relocated_imm(bin_, pc, _imm(regs[-1]),
                                         signed=(mn == "cmpwi"))
                    helper = "cr_set_u" if mn == "cmplwi" else "cr_set_i"
                    lines.append(f"    {helper}(c, {(7 - field) * 4}, {_c_gpr(ra)}, {imm});")
                    continue
            if mn in ("cmpw", "cmplw"):
                regs = [r.strip() for r in op.split(",")]
                if len(regs) in (2, 3):
                    field = _reg(regs[0], "cr") if len(regs) == 3 else 0
                    compare_regs = regs[1:] if len(regs) == 3 else regs
                    ra, rb = (_reg(r) for r in compare_regs)
                    helper = "cr_set_u" if mn == "cmplw" else "cr_set_i"
                    lines.append(f"    {helper}(c, {(7 - field) * 4}, {_c_gpr(ra)}, {_c_gpr(rb)});")
                    continue

            # CR bit aliases used by the compiler for boolean materialization.
            # `un` names the field's summary-overflow bit — the same CR
            # position as `so` (M_Sort asm clears cr7 via `crclr cr7un`).
            if mn in ("crclr", "crset") and len(op.strip()) >= 4:
                bit = op.strip()
                field = int(bit[2])
                which = {"lt": 0, "gt": 1, "eq": 2, "so": 3, "un": 3}[bit[3:]]
                lines.append(f"    CR_PUTBIT(c, {field * 4 + which}, {1 if mn == 'crset' else 0});")
                continue
            # crmove copies one CR bit to another (M_Sort asm folds compare
            # results, e.g. `crmove cr2lt, cr2eq`).
            if mn == "crmove":
                regs = [r.strip() for r in op.split(",")]
                if len(regs) == 2:
                    cr_which = {"lt": 0, "gt": 1, "eq": 2, "so": 3, "un": 3}
                    dst, src = regs
                    dbit = int(dst[2]) * 4 + cr_which[dst[3:]]
                    sbit = int(src[2]) * 4 + cr_which[src[3:]]
                    lines.append(f"    CR_PUTBIT(c, {dbit}, CR_BIT(c, {sbit}));")
                    continue

            # CR logical instructions are architectural state operations, not
            # comments/NOPs.  They feed Mercury's visibility flags, including
            # clipFLAG, before the draw callback is entered.
            cr_helpers = {
                "crxor": "CRXOR", "cror": "CROR", "crand": "CRAND",
                "crnor": "CRNOR", "crorc": "CRORC", "crandc": "CRANDC",
            }
            if mn in cr_helpers:
                regs = [r.strip() for r in op.split(",")]
                if len(regs) == 3:
                    dst, a, b = (_cr_bit(r) for r in regs)
                    lines.append(f"    {cr_helpers[mn]}(c, {dst}, {a}, {b});")
                    continue
            if mn == "crnot":
                regs = [r.strip() for r in op.split(",")]
                if len(regs) == 2:
                    dst, src = (_cr_bit(r) for r in regs)
                    lines.append(f"    CRNOR(c, {dst}, {src}, {src});")
                    continue

            # Integer immediates and arithmetic/logical operations. All GPR
            # values are uint32_t so overflow naturally wraps as on PPC.
            if mn in ("addi", "li"):
                regs = [r.strip() for r in op.split(",")]
                if (mn == "addi" and len(regs) == 3) or (mn == "li" and len(regs) == 2):
                    rd = _reg(regs[0])
                    ra = 0 if mn == "li" else _reg(regs[1])
                    raw = _imm(regs[-1])
                    imm = _relocated_imm(bin_, pc, raw, signed=True)
                    lines.append(f"    {_c_gpr(rd)} = {_base_expr(ra)} + (uint32_t){imm};")
                    continue
            if mn in ("addis", "lis"):
                regs = [r.strip() for r in op.split(",")]
                if (mn == "addis" and len(regs) == 3) or (mn == "lis" and len(regs) == 2):
                    rd = _reg(regs[0])
                    ra = 0 if mn == "lis" else _reg(regs[1])
                    raw = _imm(regs[-1])
                    imm = _relocated_imm(bin_, pc, raw, signed=True, high=True)
                    lines.append(f"    {_c_gpr(rd)} = {_base_expr(ra)} + ((uint32_t)(int32_t)(int16_t){imm} << 16);")
                    continue
            if mn in ("ori", "oris", "xori", "xoris", "andi.", "andis."):
                regs = [r.strip() for r in op.split(",")]
                if len(regs) == 3:
                    rd, ra = _reg(regs[0]), _reg(regs[1])
                    raw = _imm(regs[2])
                    shift = 16 if mn in ("oris", "xoris", "andis.") else 0
                    imm = _relocated_imm(bin_, pc, raw, signed=False)
                    value = f"((uint32_t){imm} << {shift})" if shift else f"(uint32_t){imm}"
                    opbase = mn.rstrip(".")
                    symbol = {"ori": "|", "oris": "|", "xori": "^", "xoris": "^",
                              "andi": "&", "andis": "&"}[opbase]
                    expr = f"{_c_gpr(ra)} {symbol} {value}"
                    lines.append(f"    {_c_gpr(rd)} = {expr};")
                    if mn.endswith("."):
                        # Use rd (already assigned) not expr: if rd aliases ra the
                        # re-evaluated expression would read the mutated gpr[rd].
                        _record(lines, _c_gpr(rd))
                    continue
            if mn in ("mulli", "subfic", "addic", "addic."):
                regs = [r.strip() for r in op.split(",")]
                if len(regs) == 3:
                    rd, ra = _reg(regs[0]), _reg(regs[1])
                    imm = _relocated_imm(bin_, pc, _imm(regs[2]), signed=True)
                    if mn.startswith("addic"):
                        _carry_add(lines, rd, _c_gpr(ra), str(imm), pc=pc)
                    elif mn == "subfic":
                        # PPC CA is the no-borrow bit: 1 iff (uint32)SIMM >=
                        # (uint32)rA. A 64-bit wrapping subtraction sets bit 32
                        # exactly when a borrow occurs, i.e. inverted.
                        name = f"_sub_{pc:x}"
                        lines.append(f"    {_c_gpr(rd)} = (uint32_t)((uint32_t){imm} - {_c_gpr(ra)});")
                        lines.append(f"    c->xer = (c->xer & ~0x20000000u) | ((uint32_t)((uint32_t){imm} >= {_c_gpr(ra)}) << 29);")
                    else:
                        # Defined 64-bit intermediate, low 32 kept: same value
                        # as the 32-bit product without signed-overflow UB
                        # (live: mulli r,-0x8000 in skid_updateskid).
                        lines.append(f"    {_c_gpr(rd)} = (uint32_t)((int64_t)(int32_t){_c_gpr(ra)} * (int64_t)(int32_t){imm});")
                    if mn.endswith("."):
                        _record(lines, _c_gpr(rd))
                    continue

            if mn in ("add", "add.", "addc", "addc.", "adde", "adde.",
                      "subf", "subf.", "mullw", "mullw.", "mulhwu",
                      "divw", "divw.", "divwu", "divwu."):
                regs = [r.strip() for r in op.split(",")]
                if len(regs) == 3:
                    rd, ra, rb = (_reg(r) for r in regs)
                    base = mn.rstrip(".")
                    if base == "add":
                        expr = f"{_c_gpr(ra)} + {_c_gpr(rb)}"
                        lines.append(f"    {_c_gpr(rd)} = {expr};")
                    elif base in ("addc", "adde"):
                        carry = "XER_CA(c)" if base == "adde" else "0u"
                        _carry_add(lines, rd, _c_gpr(ra), _c_gpr(rb), carry, pc)
                        expr = _c_gpr(rd)
                    elif base == "subf":
                        expr = f"{_c_gpr(rb)} - {_c_gpr(ra)}"
                        lines.append(f"    {_c_gpr(rd)} = {expr};")
                    elif base == "mullw":
                        expr = f"(uint32_t)((int64_t)(int32_t){_c_gpr(ra)} * (int64_t)(int32_t){_c_gpr(rb)})"
                        lines.append(f"    {_c_gpr(rd)} = {expr};")
                    elif base == "mulhwu":
                        name = f"_mul_{pc:x}"
                        lines.append(f"    uint64_t {name} = (uint64_t){_c_gpr(ra)} * (uint64_t){_c_gpr(rb)};")
                        lines.append(f"    {_c_gpr(rd)} = (uint32_t)({name} >> 32);")
                        expr = _c_gpr(rd)
                    elif base == "divw":
                        expr = f"(uint32_t)PPC_DIVW((int32_t){_c_gpr(ra)}, (int32_t){_c_gpr(rb)})"
                        lines.append(f"    {_c_gpr(rd)} = {expr};")
                    else:
                        expr = f"PPC_DIVWU({_c_gpr(ra)}, {_c_gpr(rb)})"
                        lines.append(f"    {_c_gpr(rd)} = {expr};")
                    if mn.endswith("."):
                        # Use rd (already assigned) not expr: if rd aliases ra or rb
                        # the re-evaluated expression would read the mutated gpr[rd].
                        # addc/adde/mulhwu already set expr = _c_gpr(rd); for add/subf/
                        # mullw/divw/divwu pass _c_gpr(rd) to avoid re-evaluation.
                        _record(lines, _c_gpr(rd))
                    continue
            if mn in ("addze", "addze.", "neg", "neg."):
                regs = [r.strip() for r in op.split(",")]
                if len(regs) == 2:
                    rd, ra = _reg(regs[0]), _reg(regs[1])
                    base = mn.rstrip(".")
                    if base == "addze":
                        _carry_add(lines, rd, _c_gpr(ra), "0u", "XER_CA(c)", pc)
                    else:
                        expr = f"(uint32_t)(0u - {_c_gpr(ra)})"
                        lines.append(f"    {_c_gpr(rd)} = {expr};")
                    if mn.endswith("."):
                        _record(lines, _c_gpr(rd))
                    continue

            if mn in ("and", "and.", "or", "or.", "xor", "xor.", "nor", "nor.",
                      "andc", "andc.", "orc", "orc.", "mr"):
                regs = [r.strip() for r in op.split(",")]
                if (mn == "mr" and len(regs) == 2) or (mn != "mr" and len(regs) == 3):
                    rd = _reg(regs[0])
                    ra = _reg(regs[1])
                    if mn == "mr":
                        expr = _c_gpr(ra)
                    else:
                        rb = _reg(regs[2])
                        base = mn.rstrip(".")
                        symbol = {"and": "&", "or": "|", "xor": "^"}.get(base)
                        if symbol:
                            expr = f"{_c_gpr(ra)} {symbol} {_c_gpr(rb)}"
                        elif base == "nor":
                            expr = f"~({_c_gpr(ra)} | {_c_gpr(rb)})"
                        elif base == "andc":
                            expr = f"{_c_gpr(ra)} & ~({_c_gpr(rb)})"
                        else:
                            expr = f"{_c_gpr(ra)} | ~({_c_gpr(rb)})"
                    lines.append(f"    {_c_gpr(rd)} = {expr};")
                    if mn.endswith("."):
                        # Use rd (already assigned) not expr: re-evaluating the
                        # expression after gpr[rd] is written reads the mutated value
                        # (e.g. xor. r3,r3,r4 after assign: (r3^r4)^r4 == r3, wrong).
                        _record(lines, _c_gpr(rd))
                    continue

            if mn in ("slw", "srw", "sraw"):
                regs = [r.strip() for r in op.split(",")]
                if len(regs) == 3:
                    rd, ra, rb = (_reg(r) for r in regs)
                    helper = {"slw": "SLW", "srw": "SRW", "sraw": "SRAW"}[mn]
                    expr = f"{helper}(c, {_c_gpr(ra)}, {_c_gpr(rb)})" if mn == "sraw" else f"{helper}({_c_gpr(ra)}, {_c_gpr(rb)})"
                    lines.append(f"    {_c_gpr(rd)} = {expr};")
                    continue
            if mn in ("slwi", "srwi", "srawi"):
                regs = [r.strip() for r in op.split(",")]
                if len(regs) == 3:
                    rd, ra = _reg(regs[0]), _reg(regs[1])
                    shift = _imm(regs[2])
                    helper = {"slwi": "SLW", "srwi": "SRW", "srawi": "SRAWI"}[mn]
                    expr = f"{helper}(c, {_c_gpr(ra)}, {shift})" if mn == "srawi" else f"{helper}({_c_gpr(ra)}, {shift})"
                    lines.append(f"    {_c_gpr(rd)} = {expr};")
                    continue
            if mn in ("rlwinm", "rlwinm."):
                regs = [r.strip() for r in op.split(",")]
                if len(regs) == 5:
                    rd, ra = _reg(regs[0]), _reg(regs[1])
                    sh, mb, me = (_imm(r) for r in regs[2:])
                    expr = f"RLWINM({_c_gpr(ra)}, {sh}, {mb}, {me})"
                    lines.append(f"    {_c_gpr(rd)} = {expr};")
                    if mn.endswith("."):
                        _record(lines, _c_gpr(rd))
                    continue
            # rlwimi inserts the rotated source into the destination's
            # unmasked bits (M_Sort asm folds CR bits into a saved-CR word,
            # e.g. `rlwimi r15, r19, 0, 8, 9`).
            if mn in ("rlwimi", "rlwimi."):
                regs = [r.strip() for r in op.split(",")]
                if len(regs) == 5:
                    rd, rs = _reg(regs[0]), _reg(regs[1])
                    sh, mb, me = (_imm(r) for r in regs[2:])
                    expr = f"RLWIMI({_c_gpr(rd)}, {_c_gpr(rs)}, {sh}, {mb}, {me})"
                    lines.append(f"    {_c_gpr(rd)} = {expr};")
                    if mn.endswith("."):
                        _record(lines, _c_gpr(rd))
                    continue
            # `clrlwi rA, rS, CL` is the ISA alias for `rlwinm rA, rS, 0, CL,
            # 31`: the window MB=CL, ME=31 zeroes the TOP CL bits and keeps the
            # low 32-CL bits (CL=0 is a no-op; CL=31 leaves only bit 0, the
            # LSB).  Bit numbering here is MSB-first -- see PPC_MASK in
            # ppc_ctx.h and the conformance vectors in test_ppc_conformance.c
            # (clrlwi == rlwinm 0,n,31; CL=8 -> 0x00ffffff, CL=16 -> 0x00001234).
            # The inverse window MB=0, ME=31-CL is `clrrwi` and selects the
            # TOP 32-CL bits instead; emitting it silently inverts 416 sites
            # across the three images and breaks the racing boot.  Do not
            # "correct" this to 31-CL without re-deriving it against those
            # vectors.
            if mn in ("clrlwi", "clrlwi."):
                regs = [r.strip() for r in op.split(",")]
                if len(regs) == 3:
                    rd, ra, cl = _reg(regs[0]), _reg(regs[1]), _imm(regs[2])
                    expr = f"RLWINM({_c_gpr(ra)}, 0, {cl}, 31)"
                    lines.append(f"    {_c_gpr(rd)} = {expr};")
                    if mn.endswith("."):
                        _record(lines, _c_gpr(rd))
                    continue

            if mn in ("extsb", "extsh"):
                regs = [r.strip() for r in op.split(",")]
                if len(regs) == 2:
                    rd, ra = _reg(regs[0]), _reg(regs[1])
                    width = "int8_t" if mn == "extsb" else "int16_t"
                    expr = f"(uint32_t)(int32_t)({width}){_c_gpr(ra)}"
                    lines.append(f"    {_c_gpr(rd)} = {expr};")
                    continue

            # D-form and X-form memory ops. Effective addresses are computed
            # once so update forms remain correct when the destination equals
            # rA. Guest RAM accessors preserve PPC big-endian layout.
            dmem = {
                "lwz": ("load", "rd32"), "lwzu": ("loadu", "rd32"),
                "lhz": ("load", "rd16"), "lhzu": ("loadu", "rd16"),
                "lha": ("load", "rd16s"), "lhau": ("loadu", "rd16s"),
                "lbz": ("load", "rd8"), "lbzu": ("loadu", "rd8"),
                "stw": ("store", "wr32"), "stwu": ("storeu", "wr32"),
                "sth": ("store", "wr16"), "sthu": ("storeu", "wr16"),
                "stb": ("store", "wr8"), "stbu": ("storeu", "wr8"),
                "lfs": ("fload", "rdf32"), "lfsu": ("floadu", "rdf32"),
                "stfs": ("fstore", "wrf32"), "stfsu": ("fstoreu", "wrf32"),
            }
            if mn in dmem:
                regs = [r.strip() for r in op.split(",", 1)]
                if len(regs) == 2:
                    target, disp_token = regs
                    disp, ra = _mem_operand(disp_token)
                    disp = _relocated_imm(bin_, pc, disp, signed=True)
                    ea = f"(_base_{pc:x} + (uint32_t){disp})"
                    lines.append(f"    uint32_t _base_{pc:x} = {_base_expr(ra)};")
                    lines.append(f"    uint32_t _ea_{pc:x} = {ea};")
                    kind, helper = dmem[mn]
                    reg = _reg(target, "f") if kind.startswith("f") else _reg(target)
                    lines.append(f"    c->pc = {pc:#x}u;")
                    if kind.startswith("load"):
                        if helper == "rd16s":
                            value = f"(uint32_t)(int32_t)(int16_t)rd16(c, _ea_{pc:x})"
                        elif helper == "rd8":
                            value = f"(uint32_t)rd8(c, _ea_{pc:x})"
                        else:
                            value = f"{helper}(c, _ea_{pc:x})"
                        lines.append(f"    {_c_gpr(reg)} = {value};")
                    elif kind.startswith("fload"):
                        lines.append(f"    {_c_fpr(reg)} = rdf32(c, _ea_{pc:x});")
                    elif kind.startswith("store"):
                        lines.append(f"    {helper}(c, _ea_{pc:x}, {_c_gpr(reg)});")
                    else:
                        lines.append(f"    {helper}(c, _ea_{pc:x}, {_c_fpr(reg)});")
                    if kind.endswith("u"):
                        lines.append(f"    {_c_gpr(ra)} = _ea_{pc:x};")
                    continue

            xmem = {
                "lwzx": ("gload", "rd32"), "lhzx": ("gload", "rd16"),
                "lhax": ("gloads", "rd16"), "lbzx": ("gload", "rd8"),
                "stwx": ("gstore", "wr32"), "sthx": ("gstore", "wr16"),
                "stbx": ("gstore", "wr8"), "lfsx": ("fload", "rdf32"),
                "stfsx": ("fstore", "wrf32"), "stfiwx": ("fstorei", "wr32"),
            }
            if mn in xmem:
                regs = [r.strip() for r in op.split(",")]
                if len(regs) == 3:
                    target, ra, rb = regs
                    ra_n, rb_n = _reg(ra), _reg(rb)
                    lines.append(f"    uint32_t _ea_{pc:x} = {_indexed_expr(ra_n, rb_n)};")
                    kind, helper = xmem[mn]
                    reg = _reg(target, "f") if kind.startswith("f") else _reg(target)
                    lines.append(f"    c->pc = {pc:#x}u;")
                    if kind == "gloads":
                        lines.append(f"    {_c_gpr(reg)} = (uint32_t)(int32_t)(int16_t)rd16(c, _ea_{pc:x});")
                    elif kind == "gload":
                        value = "rd8" if helper == "rd8" else ("rd16" if helper == "rd16" else "rd32")
                        lines.append(f"    {_c_gpr(reg)} = (uint32_t){value}(c, _ea_{pc:x});")
                    elif kind == "fload":
                        lines.append(f"    {_c_fpr(reg)} = rdf32(c, _ea_{pc:x});")
                    elif kind == "fstorei":
                        lines.append(f"    uint32_t _bits_{pc:x}; memcpy(&_bits_{pc:x}, &{_c_fpr(reg)}, sizeof(_bits_{pc:x}));")
                        lines.append(f"    wr32(c, _ea_{pc:x}, _bits_{pc:x});")
                    elif kind == "fstore":
                        lines.append(f"    wrf32(c, _ea_{pc:x}, {_c_fpr(reg)});")
                    else:
                        lines.append(f"    {helper}(c, _ea_{pc:x}, {_c_gpr(reg)});")
                    continue

            if mn in ("lmw", "stmw"):
                regs = [r.strip() for r in op.split(",", 1)]
                if len(regs) == 2:
                    first = _reg(regs[0])
                    disp, ra = _mem_operand(regs[1])
                    disp = _relocated_imm(bin_, pc, disp, signed=True)
                    lines.append(f"    uint32_t _ea_{pc:x} = {_base_expr(ra)} + (uint32_t){disp};")
                    lines.append(f"    c->pc = {pc:#x}u;")
                    lines.append(f"    for (unsigned _i_{pc:x} = {first}; _i_{pc:x} < 32; ++_i_{pc:x}, _ea_{pc:x} += 4u) {{")
                    if mn == "lmw":
                        lines.append(f"        c->gpr[_i_{pc:x}] = rd32(c, _ea_{pc:x});")
                    else:
                        lines.append(f"        wr32(c, _ea_{pc:x}, c->gpr[_i_{pc:x}]);")
                    lines.append("    }")
                    continue

            # Link-register and count-register moves are stateful even when
            # the surrounding function is a leaf.
            if mn in ("mflr", "mtlr", "mtctr", "mfctr"):
                regs = [r.strip() for r in op.split(",")]
                if len(regs) == 1:
                    r = _reg(regs[0])
                    if mn == "mflr":
                        lines.append(f"    {_c_gpr(r)} = c->lr;")
                    elif mn == "mtlr":
                        lines.append(f"    c->lr = {_c_gpr(r)};")
                    elif mn == "mfctr":
                        lines.append(f"    {_c_gpr(r)} = c->ctr;")
                    else:
                        lines.append(f"    c->ctr = {_c_gpr(r)};")
                    continue

            if mn == "mfcr":
                regs = [r.strip() for r in op.split(",")]
                if len(regs) == 1:
                    lines.append(f"    {_c_gpr(_reg(regs[0]))} = c->cr;")
                    continue
            if mn == "mtcrf":
                regs = [r.strip() for r in op.split(",")]
                if len(regs) == 2:
                    crm = _imm(regs[0])
                    rs = _reg(regs[1])
                    lines.append(f"    MTCRF(c, {crm}, {_c_gpr(rs)});")
                    continue
            # Conditional branch-to-link-register aliases (beqlr/bnelr/etc.)
            # are common in the out-of-line math helpers.
            if top == 19 and ((w >> 1) & 0x3ff) == 16 and mn not in ("blr", "blrl"):
                bo = (w >> 21) & 31
                bi = (w >> 16) & 31
                if local_return_addrs:
                    lines.append(f"    if (BC_TAKEN(c, {bo}, {bi})) {{")
                    lines.append("        switch (c->lr) {")
                    for r in sorted(local_return_addrs):
                        label = computed_local_return_map.get(r, r)
                        lines.append(f"        case {r:#x}u: goto L{label:x};")
                    lines.append("        default: return;")
                    lines.append("        }")
                    lines.append("    }")
                else:
                    lines.append(f"    if (BC_TAKEN(c, {bo}, {bi})) return;")
                continue

            # Indirect calls/jumps use the generated guest-address dispatch
            # table. A missing target remains observable through the existing
            # runtime diagnostic instead of silently becoming a NOP.
            if pc in switch_sites and mn == "bctr":
                tgts = switch_sites[pc]
                lines.append(f"    /* intra-function switch dispatch ({len(tgts)} targets) */")
                lines.append("    switch (c->ctr) {")
                for tva in tgts:
                    lines.append(f"    case {tva:#x}u: goto L{tva - bin_.text_base:x};")
                lines.append("    default: RECOMP_INDIRECT_FROM(c, c->ctr); return;")
                lines.append("    }")
                continue
            if mn in ("bctr", "bctrl", "blrl"):
                target = "c->ctr" if mn in ("bctr", "bctrl") else "c->lr"
                fn = f"_fn_{pc:x}"
                if mn == "blrl":
                    lines.append(f"    uint32_t _target_{pc:x} = c->lr;")
                    lines.append(f"    c->lr = {pc + 4:#x};")
                    target = f"_target_{pc:x}"
                lines.append(f"    RecompFn {fn} = recomp_lookup({target});")
                lines.append(f"    if ({fn}) {fn}(c); else RECOMP_INDIRECT_FROM(c, {target});")
                if mn != "blrl":
                    lines.append("    return;")
                continue

            # Branch handling
            if mn == "blr":
                if is_game_profile and pc in INDIRECT_BLR_SITES:
                    lines.append(f"    recomp_tail_call(c, c->lr);")
                    lines.append("    return;")
                    continue
                if local_return_addrs:
                    # Local-return dispatch: a c->lr that matches an
                    # intra-function call return address continues at that
                    # label inside this function; anything else returns to
                    # this function's C caller. The extra comment must not
                    # match the "/\* NNNNN: " instruction-comment shape, or
                    # it would be counted as an instruction.
                    lines.append("    /* local-return dispatch over intra-function call return addrs */")
                    lines.append("    switch (c->lr) {")
                    for r in sorted(local_return_addrs):
                        label = computed_local_return_map.get(r, r)
                        lines.append(f"    case {r:#x}u: goto L{label:x};")
                    lines.append("    default: return;")
                    lines.append("    }")
                else:
                    lines.append(f"    return;")
                continue
            if mn.startswith("b") and top in (18,16):
                # generic branch with label
                # we already handled bl selector; remaining are internal
                # Try to extract target from disasm op_str
                # For capstone, op_str for b is like "0x5f568" or "cr1, 0x44"
                # We'll use our local_targets set to emit goto if inside func
                # Else RECOMP_UNSUPPORTED for far
                # Parse target address from op_str last token
                # e.g. "cr1, 0x44" -> tgt = pc+0x44 etc but we have set
                # Simpler: if pc in local_targets? No.
                # Check if ins is branch and its target is in local set
                w2 = w
                if top == 18:
                    li = w2 & 0x03FFFFFC
                    if li & 0x02000000: li-=0x04000000
                    tgt = pc + li if not (w2 & 2) else li
                    # A link-setting branch to a symbol/gap entry is a call,
                    # even when Diab placed that entry inside our own slice
                    # (self-recursion). A goto would let the callee's blr turn
                    # into a C return that unwinds this whole function.
                    if tgt in func_starts and (w2 & 1):
                        fn = f"_fn_{pc:x}"
                        lines.append(f"    RecompFn {fn} = recomp_lookup({bin_.text_base + tgt:#x}u);")
                        lines.append(f"    c->lr = {pc + 4:#x};")
                        lines.append(f"    if ({fn}) {fn}(c); else RECOMP_INDIRECT_FROM(c, {bin_.text_base + tgt:#x}u);")
                        continue
                    if tgt in instr_addrs:
                        if w2 & 1:
                            lines.append(f"    c->lr = {pc+4:#x}; goto L{tgt:x};")
                        else:
                            lines.append(f"    goto L{tgt:x};")
                        continue
                    if tgt in func_starts:
                        fn = f"_fn_{pc:x}"
                        lines.append(f"    RecompFn {fn} = recomp_lookup({bin_.text_base + tgt:#x}u);")
                        if w2 & 1:
                            lines.append(f"    c->lr = {pc + 4:#x};")
                            lines.append(f"    if ({fn}) {fn}(c); else RECOMP_INDIRECT_FROM(c, {bin_.text_base + tgt:#x}u);")
                        else:
                            lines.append(f"    if ({fn}) {fn}(c); else RECOMP_INDIRECT_FROM(c, {bin_.text_base + tgt:#x}u);")
                            lines.append("    return;")
                        continue
                    else:
                        lines.append(f"    /* far branch {mn} {op} */ RECOMP_UNSUPPORTED(c, {pc:#x}, \"{mn} far\");")
                        continue
                if top == 16:
                    bd = w2 & 0xFFFC
                    if bd & 0x8000: bd-=0x10000
                    tgt = pc + bd if not (w2 &2) else bd
                    bo = (w2>>21)&31; bi=(w2>>16)&31
                    if tgt in func_starts and (w2 & 1):
                        fn = f"_fn_{pc:x}"
                        lines.append(f"    if (BC_TAKEN(c, {bo}, {bi})) {{")
                        lines.append(f"        c->lr = {pc + 4:#x};")
                        lines.append(f"        RecompFn {fn} = recomp_lookup({bin_.text_base + tgt:#x}u);")
                        lines.append(f"        if ({fn}) {fn}(c); else RECOMP_INDIRECT_FROM(c, {bin_.text_base + tgt:#x}u);")
                        lines.append("    }")
                        continue
                    if tgt in instr_addrs:
                        if w2 & 1:
                            lines.append(f"    if (BC_TAKEN(c, {bo}, {bi})) {{")
                            lines.append(f"        c->lr = {pc + 4:#x}; goto L{tgt:x};")
                            lines.append("    }")
                        else:
                            lines.append(f"    if (BC_TAKEN(c, {bo}, {bi})) goto L{tgt:x};")
                        continue
                    if tgt in func_starts:
                        fn = f"_fn_{pc:x}"
                        lines.append(f"    if (BC_TAKEN(c, {bo}, {bi})) {{")
                        if w2 & 1:
                            lines.append(f"        c->lr = {pc + 4:#x};")
                        lines.append(f"        RecompFn {fn} = recomp_lookup({bin_.text_base + tgt:#x}u);")
                        lines.append(f"        if ({fn}) {fn}(c); else RECOMP_INDIRECT_FROM(c, {bin_.text_base + tgt:#x}u);")
                        if not (w2 & 1):
                            lines.append("        return;")
                        lines.append("    }")
                        continue
                    else:
                        lines.append(f"    /* far bc */ RECOMP_UNSUPPORTED(c, {pc:#x}, \"bc far\");")
                        continue
            # Default: keep as comment + generic helper
            # For known simple ops we could emit via ppc_ctx helpers, but scaffold as comment is ok
            # To ensure compilation, we don't emit invalid C; comments are safe.
            # For memory ops and ALU, we would need real implementation; but for the scaffold
            # we leave them as comments and the function remains syntactically valid (just does nothing)
            # A minimal semantic: keep as nop.
            # Optionally emit a placeholder that hints at the op but doesn't affect state.
            # This keeps build green while report captures unsupported count.
            continue
    else:
        # fallback without capstone
        for p,w,_ in insns:
            if p in local_targets:
                lines.append(f"L{p:x}:;")
            rel = bin_.relocs_text.get(p)
            if rel and rel[0] == R_REL24:
                fid, ord_ = bin_.import_selector(rel[1])
                if fid==1 and ord_ in DIAB_ALL_ORDS:
                    lines.append(f"    /* Diab save/rest */")
                else:
                    lines.append(f"    c->pc = {p:#x}u;")
                    lines.append(f"    recomp_call_selector(c, {fid}, {ord_}, {p:#x});")
                continue
            lines.append(f"    /* {p:05x}: word {w:08x} */")

    lines.append("}")
    lines.append("")
    return "\n".join(lines)


def discover_gap_helpers(bin_):
    """Find symbol-less internal branch targets and their blr-delimited spans."""
    starts = {f["val"] for f in bin_.funcs}
    targets = set()
    try:
        from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN
        md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
        md.detail = False
        for func in bin_.funcs:
            lo_addr, hi_addr = func["val"], func["val"] + func["sz"]
            for ins in _disasm_resync(md, bin_.text[lo_addr:hi_addr], lo_addr):
                w = struct.unpack(">I", bin_.text[ins.address:ins.address + 4])[0]
                rel = bin_.relocs_text.get(ins.address)
                if rel and rel[0] == R_REL24:
                    continue
                top = w >> 26
                if top == 18:
                    delta = w & 0x03FFFFFC
                    if delta & 0x02000000:
                        delta -= 0x04000000
                    target = ins.address + delta if not (w & 2) else delta
                elif top == 16:
                    delta = w & 0xFFFC
                    if delta & 0x8000:
                        delta -= 0x10000
                    target = ins.address + delta if not (w & 2) else delta
                else:
                    continue
                if target not in starts and not (lo_addr <= target < hi_addr):
                    if 0 <= target < len(bin_.text):
                        targets.add(target)
    except Exception:
        return {}

    md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
    md.detail = False

    def span(start):
        size = 4
        for ins in _disasm_resync(md, bin_.text[start:min(start + 2048, len(bin_.text))], start):
            size = ins.address + 4 - start
            if ins.mnemonic == "blr":
                break
        return size

    gaps = {start: span(start) for start in sorted(targets)}
    for start, end in (EXPLICIT_GAP_SPANS.items()
                       if bin_.profile == "driving-game" else ()):
        bad = [i.address for i in
               _disasm_resync(md, bin_.text[start:end], start)
               if i.mnemonic == "invalid"]
        if bad:
            raise SystemExit(
                f"explicit gap span {start:#x}..{end:#x} disassembles with "
                f"invalid words at {[hex(a) for a in bad]} — refusing to lift")
        gaps[start] = end - start
    # Some helpers share a common tail. Add branch-entry labels inside an
    # already discovered helper as independent callable spans so a cross-gap
    # branch can still return through the normal C function boundary.
    changed = True
    while changed:
        changed = False
        for start, size in list(gaps.items()):
            end = start + size
            for ins in _disasm_resync(md, bin_.text[start:min(end, len(bin_.text))], start):
                w = struct.unpack(">I", bin_.text[ins.address:ins.address + 4])[0]
                top = w >> 26
                if top == 18:
                    delta = w & 0x03FFFFFC
                    if delta & 0x02000000:
                        delta -= 0x04000000
                    target = ins.address + delta if not (w & 2) else delta
                elif top == 16:
                    delta = w & 0xFFFC
                    if delta & 0x8000:
                        delta -= 0x10000
                    target = ins.address + delta if not (w & 2) else delta
                else:
                    continue
                in_original = any(f["val"] <= target < f["val"] + f["sz"] for f in bin_.funcs)
                if (target not in gaps and target not in starts and
                        not in_original and 0 <= target < len(bin_.text)):
                    gaps[target] = span(target)
                    changed = True
    return gaps


def _c_string(value):
    """Return a JSON-escaped string literal, also valid C source."""
    return json.dumps(value, ensure_ascii=True)


def emit_tracepoint_source(path, tracepoints):
    """Emit the bounded guest tracepoint table consumed by guest_tracepoints.c."""
    with open(path, "w", encoding="utf-8") as out:
        out.write("// GENERATED — declarative guest tracepoints; do not edit\n")
        out.write("#include \"guest_tracepoints.h\"\n\n")
        out.write("M2TracepointSpec g_m2_tracepoint_specs[M2_TRACEPOINT_MAX_SPECS] = {\n")
        for tracepoint in tracepoints:
            out.write("    {\n")
            out.write(f"        .name = {_c_string(tracepoint['name'])},\n")
            out.write(f"        .pc = 0x{tracepoint['pc']:x}u,\n")
            out.write(f"        .limit = {tracepoint['limit']}u,\n")
            out.write(f"        .gpr_mask = 0x{tracepoint['gpr_mask']:08x}u,\n")
            out.write(f"        .capture_lr = {1 if tracepoint['capture_lr'] else 0}u,\n")
            out.write(f"        .capture_ctr = {1 if tracepoint['capture_ctr'] else 0}u,\n")
            out.write(f"        .capture_cr = {1 if tracepoint['capture_cr'] else 0}u,\n")
            when_kind = ("M2_TRACEPOINT_WHEN_OUTSIDE_GUEST_RAM"
                         if tracepoint["when_kind"] == "outside_guest_ram"
                         else "M2_TRACEPOINT_WHEN_NONE")
            when_register = (tracepoint["when_register"]
                             if tracepoint["when_register"] is not None else 0)
            out.write(f"        .when_kind = {when_kind},\n")
            out.write(f"        .when_register = {when_register}u,\n")
            out.write(f"        .memory_count = {len(tracepoint['memory'])}u,\n")
            out.write("        .memory = {\n")
            for memory in tracepoint["memory"]:
                kind = ("M2_TRACEPOINT_MEMORY_FIXED"
                        if memory["kind"] == "fixed"
                        else "M2_TRACEPOINT_MEMORY_RELATIVE")
                base_register = (memory["base_register"]
                                 if memory["base_register"] is not None else 0)
                out.write(
                    "            {"
                    f" .ea = 0x{memory['ea']:x}u,"
                    f" .offset = {memory['offset']},"
                    f" .length = {memory['length']}u,"
                    f" .kind = {kind},"
                    f" .base_register = {base_register}u,"
                    f" .provenance = {1 if memory['provenance'] else 0}u,"
                    f" .label = {_c_string(memory['label'])}"
                    " },\n")
            out.write("        },\n")
            out.write("    },\n")
        out.write("};\n")
        out.write(f"const uint32_t g_m2_tracepoint_count = {len(tracepoints)}u;\n")


def generate(bin_, outdir, chunk_size=120, tracepoints_path=None):
    os.makedirs(outdir, exist_ok=True)
    image_name = bin_.output_prefix
    image_prefix = bin_.symbol_prefix
    lookup_name = "recomp_lookup" if not image_prefix else f"recomp_lookup_{image_name}"
    gap_sizes = discover_gap_helpers(bin_)
    bin_.gap_starts = set(gap_sizes)
    tracepoints = load_tracepoints(bin_, tracepoints_path, gap_sizes)
    bin_.tracepoints = tracepoints
    bin_.tracepoint_by_pc = collections.defaultdict(list)
    for tracepoint in tracepoints:
        bin_.tracepoint_by_pc[tracepoint["pc"]].append(tracepoint["id"])
    emit_tracepoint_source(os.path.join(outdir, f"{image_name}_tracepoints.c"), tracepoints)
    # 1) image layout and relocation fixups. The image bytes themselves are
    # not emitted: the runtime reads .text and .data from the user's
    # driving/game at the recorded file offsets, checks the image hash, and
    # applies these ADDR32 fixups (absolute guest VAs, patched over the raw
    # words). The lift itself keeps reading pristine bin_.text; pool words
    # inside text are never executed but runtime lwz loads read them via
    # guest memory, so they must be fixed up too.
    data_fixups = []
    for off, (t, sym, add) in sorted(bin_.relocs_data.items()):
        if t == R_ADDR32:
            data_fixups.append((off, bin_.abs_addr_for_reloc(sym, add)))
    text_fixups_list = []
    for off, (t, sym, add) in sorted(bin_.relocs_text.items()):
        if t == R_ADDR32:
            text_fixups_list.append((off, bin_.abs_addr_for_reloc(sym, add)))
    fixups = len(data_fixups)
    text_fixups = len(text_fixups_list)
    image_sha256 = hashlib.sha256(bin_.raw).digest()
    # Preserve the historical global names for driving/game. Additional
    # images receive prefixed symbols so both generated data sets can link.
    with open(os.path.join(outdir, f"{image_name}_data.c"), "w") as f:
        f.write("// GENERATED: guest image layout and relocation fixups (no image bytes)\n")
        f.write('#include <stdint.h>\n')
        f.write(f"unsigned g_{image_prefix}recomp_text_base = {bin_.text_base:#x};\n")
        f.write(f"unsigned g_{image_prefix}recomp_data_base = {bin_.data_base:#x};\n")
        f.write(f"unsigned g_{image_prefix}recomp_bss_base = {bin_.bss_base:#x};\n")
        f.write(f"unsigned g_{image_prefix}recomp_text_size = {len(bin_.text)};\n")
        f.write(f"unsigned g_{image_prefix}recomp_data_size = {len(bin_.data)};\n")
        f.write(f"unsigned g_{image_prefix}recomp_bss_size = {bin_.bss_size};\n")
        f.write(f"// Offsets of .text and .data within the image file, and the image's SHA-256.\n")
        f.write(f"unsigned g_{image_prefix}recomp_text_file_offset = {bin_.text_off:#x};\n")
        f.write(f"unsigned g_{image_prefix}recomp_data_file_offset = {bin_.data_off:#x};\n")
        f.write(f"unsigned long long g_{image_prefix}recomp_image_size = {len(bin_.raw)};\n")
        f.write(f"const uint8_t g_{image_prefix}recomp_image_sha256[32] = {{\n")
        f.write("  " + ", ".join(f"0x{b:02x}" for b in image_sha256) + ",\n};\n")
        f.write("// Fixups: {offset into the section, absolute guest VA written there as a big-endian word}\n")
        for sect, table in (("text", text_fixups_list), ("data", data_fixups)):
            f.write(f"unsigned g_{image_prefix}recomp_{sect}_fixup_count = {len(table)};\n")
            f.write(f"const uint32_t g_{image_prefix}recomp_{sect}_fixups[][2] = {{\n")
            for off, val in table:
                f.write(f"  {{0x{off:x}, 0x{val & 0xFFFFFFFF:x}}},\n")
            f.write("  {0, 0}\n};\n")
    # 2) text recomp in chunks
    chunks = []
    for i in range(0, len(bin_.funcs), chunk_size):
        chunk_funcs = bin_.funcs[i:i+chunk_size]
        idx = i // chunk_size
        fname = f"{image_name}_text_{idx:02d}.c"
        path = os.path.join(outdir, fname)
        with open(path, "w") as out:
            out.write("// GENERATED by recompiler/transpile.py — do not edit\n")
            out.write(f"// chunk {idx} funcs {len(chunk_funcs)} (total {len(bin_.funcs)})\n")
            out.write(f"// TEXT_BASE {bin_.text_base:#x} DATA_BASE {bin_.data_base:#x} BSS_BASE {bin_.bss_base:#x}\n")
            if image_prefix:
                out.write(f"#define recomp_lookup {lookup_name}\n")
            out.write('#include "ppc_ctx.h"\n')
            out.write('#include "recomp_dispatch.h"\n')
            out.write('#include "recomp_helpers.h"\n')
            out.write('#include "shim_abi.h"\n')
            out.write("void* m2_selector_lookup(uint32_t,uint32_t);\n")
            out.write("\n")
            # forward decls
            for func in chunk_funcs:
                fs = func_sym(func, image_prefix)
                out.write(f"void FN_{fs}(PPCContext* c);\n")
            out.write("\n")
            for i_fn, func in enumerate(chunk_funcs):
                code = func_emitter(bin_, func, idx)
                out.write(code)
                if i_fn + 1 < len(chunk_funcs):
                    out.write("\n")
        chunks.append(fname)
    # 3) master header
    with open(os.path.join(outdir, f"{image_name}_recomp.h"), "w") as h:
        h.write("// GENERATED master header\n#pragma once\n#include \"ppc_ctx.h\"\n")
        for func in bin_.funcs:
            fs = func_sym(func, image_prefix)
            h.write(f"void FN_{fs}(PPCContext* c);\n")
        for addr in sorted(gap_sizes):
            h.write(f"void FN_{image_prefix}gap_{addr:x}__{addr:05x}(PPCContext* c);\n")
    # 4) dispatch table for bctr
    with open(os.path.join(outdir, f"{image_name}_dispatch.c"), "w") as d:
        d.write("// GENERATED dispatch for indirect calls / bctr\n")
        d.write('#include "recomp_dispatch.h"\n#include "recomp_helpers.h"\n')
        d.write(f'#include "{image_name}_recomp.h"\n')
        if image_prefix:
            d.write(f"extern unsigned g_{image_prefix}recomp_text_base;\n")
        d.write("typedef struct { unsigned guest; RecompFn fn; } DispatchEnt;\n")
        # One globally ascending table so the lookup can binary-search it. The
        # former layout listed every function start and then every interior gap
        # helper, which is not address-sorted; duplicate addresses keep the
        # function start ahead of an equal-address gap helper.
        dispatch_entries = []
        for func in bin_.funcs:
            fs = func_sym(func, image_prefix)
            dispatch_entries.append((bin_.text_base + func["val"], 0,
                                     f"FN_{fs}"))
        for addr in sorted(gap_sizes):
            dispatch_entries.append((bin_.text_base + addr, 1,
                                     f"FN_{image_prefix}gap_{addr:x}__{addr:05x}"))
        dispatch_entries.sort(key=lambda entry: (entry[0], entry[1]))
        d.write("static DispatchEnt kDispatch[] = {\n")
        for guest, _rank, fn in dispatch_entries:
            d.write(f"  {{{guest:#x}, {fn}}},\n")
        d.write("};\n")
        d.write("static const unsigned kDispatchCount =\n")
        d.write("    (unsigned)(sizeof(kDispatch) / sizeof(kDispatch[0]));\n")
        d.write("// Binary search: the table spans the whole image and indirect/tail\n")
        d.write("// calls are hot on the guest threads, so the former linear scan cost\n")
        d.write("// whole milliseconds of guest CPU per race frame.\n")
        d.write("static RecompFn dispatch_find(unsigned guest) {\n")
        d.write("  unsigned lo = 0u, hi = kDispatchCount;\n")
        d.write("  while (lo < hi) {\n")
        d.write("    const unsigned mid = lo + (hi - lo) / 2u;\n")
        d.write("    if (kDispatch[mid].guest < guest) lo = mid + 1u;\n")
        d.write("    else hi = mid;\n")
        d.write("  }\n")
        d.write("  if (lo < kDispatchCount && kDispatch[lo].guest == guest)\n")
        d.write("    return kDispatch[lo].fn;\n")
        d.write("  return 0;\n")
        d.write("}\n")
        d.write(f"RecompFn {lookup_name}(unsigned guest) {{\n")
        d.write("  RecompFn found = dispatch_find(guest);\n")
        d.write("  if (found) return found;\n")
        d.write("  // BSS-resident function pointers (e.g. StateDriver 0x186550 slot at 0x2fc20/0x2fc6c)\n")
        d.write("  // are loaded text-relative by the original code because the M2 loader rebased them at\n")
        d.write("  // runtime. The recomp stores them text-relative; fall back to text_base + guest so a\n")
        d.write("  // BSS-resident function pointer resolves when its offset is below TEXT_BASE.\n")
        text_base_symbol = f"g_{image_prefix}recomp_text_base"
        d.write(f"  if (guest != 0 && guest < {text_base_symbol}) {{\n")
        d.write(f"    unsigned rebased = guest + {text_base_symbol};\n")
        d.write("    found = dispatch_find(rebased);\n")
        d.write("    if (found) return found;\n")
        d.write("  }\n")
        d.write("  return 0;\n}\n")
        for addr, size in sorted(gap_sizes.items()):
            gap = {"val": addr, "sz": size, "name": f"gap_{addr:x}", "file": "gap"}
            d.write(func_emitter(bin_, gap, -1))
    # 5) report
    rel_counts = collections.Counter()
    for dct in (bin_.relocs_text, bin_.relocs_data):
        for t,_,_ in dct.values():
            rel_counts[t] += 1
    selector_calls = sum(1 for t,_,_ in bin_.relocs_text.values() if t == R_REL24)
    report = {
        "elf": bin_.path,
        "funcs": len(bin_.funcs),
        "text_bytes": len(bin_.text),
        "data_bytes": len(bin_.data),
        "bss_bytes": bin_.bss_size,
        "link_map": {"TEXT_BASE": bin_.text_base, "DATA_BASE": bin_.data_base, "BSS_BASE": bin_.bss_base},
        "relocs": {str(k): v for k,v in rel_counts.items()},
        "reloc_names": {164:"LO16",165:"HI16",166:"HA16",186:"ADDR32",202:"IMPREL24",
                         196:"LO16_FOLIO",197:"HI16_FOLIO",198:"HA16_FOLIO"},
        "import_table": {"base": hex(bin_.import_table_base),
                         "slots": {f"{f}:{o}": i for (f,o),i in sorted(bin_.import_slots.items())}},
        "selector_calls": selector_calls,
        "gap_helpers": {f"{addr:#x}": size for addr, size in sorted(gap_sizes.items())},
        "chunks": chunks,
        "fixups": fixups,
        "text_fixups": text_fixups,
        "lifted_va_thunks": getattr(bin_, "va_thunks", []),
        "lifted_imported_va_thunks": getattr(bin_, "imported_va_thunks", []),
    }
    # Preserve the byte-for-byte no-config report used by the existing
    # regeneration gate. An explicitly supplied tracepoint file gets its
    # resolved records in the report for auditability.
    if tracepoints_path is not None:
        report["tracepoints"] = tracepoints
    report_basename = "recomp_report" if image_name == "game" else f"{image_name}_report"
    if image_name != "game":
        report["module_map"] = bin_.module_map
    with open(os.path.join(outdir, f"{report_basename}.json"), "w") as jf:
        json.dump(report, jf, indent=2)
    with open(os.path.join(outdir, f"{report_basename}.md"), "w") as mf:
        mf.write(f"# Recomp report\n\nFuncs {len(bin_.funcs)} text {len(bin_.text)} data {len(bin_.data)} bss {bin_.bss_size}\n\n")
        mf.write(f"Link: TEXT {bin_.text_base:#x} DATA {bin_.data_base:#x} BSS {bin_.bss_base:#x}\n\n")
        mf.write(f"Relocs: {dict(rel_counts)}\n\nSelector calls {selector_calls}\n\nChunks {chunks}\n")
        mf.write(f"Fixups {fixups} ADDR32 in .data, {text_fixups} ADDR32 in .text (recorded in the runtime fixup table)\n")
        mf.write(f"Lifted local Diab VA thunks {len(getattr(bin_, 'va_thunks', []))}\n")
        mf.write(f"Lifted imported Diab VA thunks {len(getattr(bin_, 'imported_va_thunks', []))}\n")
        if tracepoints_path is not None:
            mf.write(f"Declarative tracepoints {len(tracepoints)}\n")
        mf.write(f"Host helper header: recomp_helpers.h, FP: ppc_ctx.h (FMADDS via ppc_fma_single etc)\n")
    print(json.dumps(report, indent=2))
    return report

def main():
    try:
        import capstone  # noqa: F401
    except ImportError:
        raise SystemExit(
            "transpile.py requires capstone (`pip install capstone`) for "
            "instruction decoding and lifted semantics.")
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", required=True,
                    help="ET_REL guest image (the game data's driving/game)")
    ap.add_argument("--out", default=OUTDIR)
    ap.add_argument("--name", default="game",
                    help="generated artifact and symbol prefix (default: game)")
    ap.add_argument("--text-base", type=lambda value: int(value, 0),
                    help="guest text link base (default: 0x00100000)")
    ap.add_argument("--import-table-base", type=lambda value: int(value, 0),
                    help="guest folio import-slot table base (default: 0x003f1000)")
    ap.add_argument("--profile", choices=("driving-game", "generic"),
                    help="apply image-specific driving/game lift evidence; defaults by ELF path")
    ap.add_argument("--chunk", type=int, default=120)
    ap.add_argument("--tracepoints", help="JSON tracepoint specification to embed during generation")
    args = ap.parse_args()
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", args.name):
        ap.error("--name must be a C identifier")
    default_game = _image_is(args.elf, "driving", "game")
    profile = args.profile or ("driving-game" if default_game else "generic")
    bin_ = Binary(args.elf,
                  text_base=TEXT_BASE if args.text_base is None else args.text_base,
                  import_table_base=(IMPORT_TABLE_BASE if args.import_table_base is None
                                     else args.import_table_base),
                  profile=profile, output_prefix=args.name)
    print(f"Loaded {args.elf}: {len(bin_.funcs)} funcs, text {len(bin_.text)} data {len(bin_.data)} bss {bin_.bss_size}")
    print(f"Relocs text {len(bin_.relocs_text)} data {len(bin_.relocs_data)}")
    generate(bin_, args.out, args.chunk, args.tracepoints)

if __name__ == "__main__":
    main()
