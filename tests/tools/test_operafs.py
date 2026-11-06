"""Tests for tools/operafs.py using a small synthetic image (no game data)."""
import os
import struct
import subprocess
import sys
import threading

import pytest

TOOLS = os.path.join(os.path.dirname(__file__), "..", "..", "tools")
sys.path.insert(0, TOOLS)
import operafs  # noqa: E402

S = 2048


def entry(flags, type_code, byte_count, block_count, name, avatar):
    raw = struct.pack(">IIIIIIII", flags, 0, int.from_bytes(type_code.ljust(4, b"\0"), "big"),
                      S, byte_count, block_count, 0, 0)
    raw += name.encode("latin-1").ljust(32, b"\0")
    return raw + struct.pack(">II", 0, avatar)


def dir_block(entries, nxt=-1, prev=-1):
    body = b"".join(entries)
    head = struct.pack(">iiIII", nxt, prev, 0, 0x14 + len(body), 0x14)
    return (head + body).ljust(S, b"\0")


def build_image(path, payload):
    blocks = [b""] * 6
    vol = bytearray(S)
    vol[0] = 1
    vol[1:6] = b"ZZZZZ"
    vol[6] = 1
    vol[0x28:0x30] = b"TESTDISC"
    struct.pack_into(">IIIIII", vol, 0x4C, S, 6, 0, 1, S, 0)
    struct.pack_into(">I", vol, 0x64, 1)
    blocks[0] = bytes(vol)
    blocks[1] = dir_block([
        entry(0x2, b"txt", len(payload), 1, "a.txt", 3),
        entry(0xC0000007, b"*dir", S, 1, "sub", 2),
    ])
    blocks[2] = dir_block([entry(0x40000002, b"bin", 3, 1, "b!.bin", 4)])
    blocks[3] = payload.ljust(S, b"\0")
    blocks[4] = b"xyz".ljust(S, b"\0")
    blocks[5] = b""
    with open(path, "wb") as f:
        f.write(b"".join(blocks))


def build_two_block_root_image(path):
    """Root directory spans blocks 1-2; each block holds one file entry."""
    blocks = [b""] * 6
    vol = bytearray(S)
    vol[0] = 1
    vol[1:6] = b"ZZZZZ"
    vol[6] = 1
    vol[0x28:0x30] = b"TESTDISC"
    struct.pack_into(">IIIIII", vol, 0x4C, S, 6, 0, 2, S, 0)
    struct.pack_into(">I", vol, 0x64, 1)
    blocks[0] = bytes(vol)
    blocks[1] = dir_block([
        entry(0x40000002, b"txt", 5, 1, "first.txt", 3),
    ], nxt=1)
    blocks[2] = dir_block([
        entry(0x80000002, b"txt", 4, 1, "second.txt", 4),
    ], nxt=-1, prev=0)
    blocks[3] = b"first".ljust(S, b"\0")
    blocks[4] = b"next".ljust(S, b"\0")
    with open(path, "wb") as f:
        f.write(b"".join(blocks))


def walk_promptly(iso, timeout=5.0):
    """Walk an image in a worker thread so a traversal bug fails, not hangs."""
    outcome = {}

    def target():
        try:
            with operafs.Volume(str(iso)) as vol:
                outcome["entries"] = list(vol.walk())
        except BaseException as exc:  # noqa: BLE001 - re-raised below
            outcome["error"] = exc

    worker = threading.Thread(target=target, daemon=True)
    worker.start()
    worker.join(timeout)
    assert not worker.is_alive(), (
        "directory walk did not terminate within %.1f s" % timeout)
    if "error" in outcome:
        raise outcome["error"]
    return outcome["entries"]


def run_cli(*args, timeout=10):
    return subprocess.run(
        [sys.executable, os.path.join(TOOLS, "operafs.py")] + list(args),
        capture_output=True, text=True, timeout=timeout)


def test_extract_roundtrip(tmp_path):
    iso = tmp_path / "t.iso"
    build_image(iso, b"hello opera")
    # The root directory avatar points at block 1.
    data = bytearray(iso.read_bytes())
    struct.pack_into(">I", data, 0x64, 1)
    iso.write_bytes(bytes(data))
    out = tmp_path / "out"
    assert operafs.extract(str(iso), str(out)) == 2
    assert (out / "a.txt").read_bytes() == b"hello opera"
    assert (out / "sub" / "b!21.bin").read_bytes() == b"xyz"


def test_rejects_non_opera(tmp_path):
    bad = tmp_path / "bad.iso"
    bad.write_bytes(b"\0" * 8192)
    with pytest.raises(operafs.OperaFSError):
        operafs.Volume(str(bad))


def test_rejects_path_traversal():
    with pytest.raises(operafs.OperaFSError):
        operafs.safe_join("/tmp/x", "../evil")


def test_lists_and_extracts_valid_multi_block_directory(tmp_path):
    iso = tmp_path / "multi.iso"
    build_two_block_root_image(iso)

    entries = walk_promptly(iso)
    assert [e.path for e in entries] == ["first.txt", "second.txt"]

    out = tmp_path / "out"
    assert operafs.extract(str(iso), str(out)) == 2
    assert (out / "first.txt").read_bytes() == b"first"
    assert (out / "second.txt").read_bytes() == b"next"


def test_rejects_self_linked_directory_block(tmp_path):
    iso = tmp_path / "self-link.iso"
    build_image(iso, b"hello opera")
    data = bytearray(iso.read_bytes())
    # Root directory block 1 points at itself instead of terminating.
    struct.pack_into(">i", data, S, 0)
    iso.write_bytes(bytes(data))

    with pytest.raises(operafs.OperaFSError, match="cyclic directory block chain"):
        walk_promptly(iso)


def test_rejects_multi_block_directory_cycle(tmp_path):
    iso = tmp_path / "cycle.iso"
    build_two_block_root_image(iso)
    data = bytearray(iso.read_bytes())
    # Second root directory block links back to the first: 0 -> 1 -> 0.
    struct.pack_into(">i", data, 2 * S, 0)
    iso.write_bytes(bytes(data))

    with pytest.raises(operafs.OperaFSError, match="cyclic directory block chain"):
        walk_promptly(iso)


def test_cyclic_directory_cli_diagnostic(tmp_path):
    iso = tmp_path / "cycle-cli.iso"
    build_image(iso, b"hello opera")
    data = bytearray(iso.read_bytes())
    struct.pack_into(">i", data, S, 0)
    iso.write_bytes(bytes(data))

    for command in (("list", str(iso)),
                    ("extract", str(iso), str(tmp_path / "out"))):
        result = run_cli(*command)
        assert result.returncode == 1
        assert "operafs: cyclic directory block chain" in result.stderr
        assert "Traceback" not in result.stderr
