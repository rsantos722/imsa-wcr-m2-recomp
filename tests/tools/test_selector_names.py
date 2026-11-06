"""The selector name tables built from the names file and the guest images."""
import json
import os
import pathlib
import re
import subprocess
import sys

import pytest

REPO = pathlib.Path(__file__).resolve().parents[2]
GENERATOR = REPO / "tools" / "gen_selector_names.py"
NAMES = REPO / "shims" / "abi" / "selector-names.json"
ROW = re.compile(r'\{0x([0-9A-F]{8})u, (?:"([^"]+)"|nullptr), (\d+)\}')


def generate(out, *extra):
    subprocess.run([sys.executable, str(GENERATOR), "--out", str(out), *extra],
                   check=True, capture_output=True)
    return out.read_text(encoding="utf-8")


def table(text, symbol):
    body = re.search(r"%s\[\] = \{(.*?)\};" % symbol, text, re.S).group(1)
    rows = [(int(sel, 16), name or None, int(count))
            for sel, name, count in ROW.findall(body)]
    count = int(re.search(r"%sCount = (\d+);" % symbol, text).group(1))
    assert count == len(rows)
    return rows


def test_names_file_is_well_formed():
    data = json.loads(NAMES.read_text(encoding="utf-8"))
    folios = set(data["folios"].values())
    assert set(data["names"]) <= folios
    for ordinals in data["names"].values():
        assert all(int(o) >= 0 and name for o, name in ordinals.items())


def test_without_game_data_lists_every_name(tmp_path):
    text = generate(tmp_path / "selector_names.gen.h")
    data = json.loads(NAMES.read_text(encoding="utf-8"))
    expected = sum(len(v) for v in data["names"].values())
    for symbol in ("kSelectorNames", "kSelectorNamesShooter2d"):
        rows = table(text, symbol)
        assert len(rows) == expected
        assert [r[0] for r in rows] == sorted(r[0] for r in rows)
        assert all(name and count == 0 for _, name, count in rows)


def test_supported_disc_tables(tmp_path):
    root = os.environ.get("IMSA_DATA_DIR")
    if not root or not (pathlib.Path(root) / "driving" / "game").is_file():
        pytest.skip("IMSA_DATA_DIR does not point at extracted game data")
    pytest.importorskip("capstone")
    text = generate(tmp_path / "selector_names.gen.h", "--game-data", root)
    # Pinned from the supported disc: selector and call-site totals per image,
    # and every imported selector has a public name.
    for symbol, selectors, sites in (("kSelectorNames", 214, 2962),
                                     ("kSelectorNamesShooter2d", 134, 717)):
        rows = table(text, symbol)
        assert len(rows) == selectors
        assert sum(r[2] for r in rows) == sites
        assert all(name for _, name, _ in rows)
