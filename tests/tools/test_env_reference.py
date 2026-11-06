"""The environment catalog lists every variable the code reads, and the
generated reference page matches it."""
import pathlib
import re
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
CATALOG = REPO / "launcher" / "flags_catalog.h"
GENERATOR = REPO / "tools" / "gen_env_reference.py"
SOURCE_DIRS = ("host", "launcher", "runtime", "shims")
NAME = re.compile(r'"((?:M2|IMSA)_[A-Z0-9_]+)"')


def catalog_names():
    text = CATALOG.read_text(encoding="utf-8")
    return set(re.findall(r'^\s*\{"((?:M2|IMSA)_[A-Z0-9_]+)"', text, re.M))


def source_names():
    names = set()
    for top in SOURCE_DIRS:
        for path in (REPO / top).rglob("*"):
            if path.suffix not in {".c", ".cpp", ".h", ".mm"} or path == CATALOG:
                continue
            names |= set(NAME.findall(path.read_text(encoding="utf-8",
                                                     errors="ignore")))
    return names


def test_every_variable_is_cataloged():
    missing = sorted(source_names() - catalog_names())
    assert not missing, (
        "add these variables to launcher/flags_catalog.h, then run "
        f"python3 tools/gen_env_reference.py: {missing}")


def test_catalog_has_no_stale_rows():
    stale = sorted(catalog_names() - source_names())
    assert not stale, f"launcher/flags_catalog.h lists unused variables: {stale}"


def test_reference_page_is_current():
    result = subprocess.run([sys.executable, str(GENERATOR), "--check"],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
