#!/usr/bin/env python3
"""Generate selector_names.gen.h from the user's guest images.

Every external call in an M2 guest image is a REL24 relocation whose symbol
field packs the image's `.imp3do` module index and the export ordinal. The
recompiler's `Binary` loader maps that to a host selector
`(folio_id << 16) | vector_ordinal`. This script counts those relocations in
`driving/game` and `shooter2d/shooter2d` and names each selector from
`shims/abi/selector-names.json` (public Portfolio OS export names), producing
the compact C++ table the host shim layer uses for logging and the boot
census.

One table is emitted per image because the census is per process and the two
images disagree about which selectors exist and how often they are called: a
merged table would either drop font/gstate or attach the wrong call count to a
kernel ordinal. `folio_set_name_source()` selects the booted image's table.

Without `--game-data` (builds without the disc) both tables list every named
selector with a call count of zero.

Usage:
    python3 tools/gen_selector_names.py --out FILE [--game-data DIR]
"""
from __future__ import annotations

import argparse
import collections
import json
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_NAMES = REPO / "shims" / "abi" / "selector-names.json"

# (table symbol, image path under the game data root, label, Binary arguments).
# The bases match cmake/ImsaGenerate.cmake; only the import mapping matters
# for selectors, but the loader takes the full set.
IMAGES = [
    ("kSelectorNames", ("driving", "game"), "driving/game",
     dict(text_base=0x100000, import_table_base=0x3F1000,
          profile="driving-game", output_prefix="game")),
    ("kSelectorNamesShooter2d", ("shooter2d", "shooter2d"), "shooter2d",
     dict(text_base=0x2C0000, import_table_base=0x3F2000,
          profile="generic", output_prefix="shooter2d")),
]

HEADER = """\
// GENERATED FILE - do not edit by hand.
// Built by tools/gen_selector_names.py from {source} and
// shims/abi/selector-names.json.
#pragma once

#include <cstddef>
#include <cstdint>

namespace m2host {{

struct SelectorName {{
    uint32_t sel;
    const char* name;  // nullptr = no public name for this ordinal
    int call_count;    // static call sites in this image
}};
"""

FOOTER = """
}  // namespace m2host
"""


def load_names(path: pathlib.Path) -> dict[int, str]:
    data = json.loads(path.read_text(encoding="utf-8"))
    folio_ids = {name: int(fid) for fid, name in data["folios"].items()}
    names = {}
    for folio, ordinals in data["names"].items():
        for ordinal, name in ordinals.items():
            names[(folio_ids[folio] << 16) | int(ordinal)] = name
    return names


def image_counts(elf: pathlib.Path, binary_args: dict) -> collections.Counter:
    sys.path.insert(0, str(REPO / "recompiler"))
    import transpile

    image = transpile.Binary(str(elf), **binary_args)
    counts = collections.Counter()
    for kind, sym, _addend in image.relocs_text.values():
        if kind == transpile.R_REL24:
            folio, ordinal = image.import_selector(sym)
            counts[(folio << 16) | ordinal] += 1
    return counts


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=pathlib.Path, required=True)
    ap.add_argument("--game-data", type=pathlib.Path,
                    help="extracted disc root; omit for builds without game data")
    ap.add_argument("--names", type=pathlib.Path, default=DEFAULT_NAMES)
    args = ap.parse_args()

    names = load_names(args.names)
    source = "the guest images' relocations" if args.game_data else "no guest images"
    out = [HEADER.format(source=source)]
    for symbol, parts, label, binary_args in IMAGES:
        if args.game_data:
            elf = args.game_data.joinpath(*parts)
            if not elf.is_file():
                ap.error(f"guest image not found: {elf}")
            counts = image_counts(elf, binary_args)
            note = f"{len(counts)} selectors from {sum(counts.values())} call sites"
        else:
            counts = collections.Counter({sel: 0 for sel in names})
            note = "no game data; every named selector, no call counts"
        out.append(f"\n// {label}: {note}.\n")
        out.append(f"inline constexpr SelectorName {symbol}[] = {{\n")
        for sel in sorted(counts):
            nm = names.get(sel)
            text = f'"{nm}"' if nm else "nullptr"
            out.append(f"    {{0x{sel:08X}u, {text}, {counts[sel]}}},\n")
        out.append("};\n")
        out.append(f"inline constexpr size_t {symbol}Count = {len(counts)};\n")
    out.append(FOOTER)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    # Force LF so the header is byte-identical on Windows.
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
