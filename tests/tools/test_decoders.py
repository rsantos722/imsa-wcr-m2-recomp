"""Behaviour tests for the format tools. No game data or network required."""
import os
import struct
import subprocess
import sys

import pytest

TOOLS = os.path.join(os.path.dirname(__file__), "..", "..", "tools")
sys.path.insert(0, TOOLS)

import decode_cbd2  # noqa: E402
import decode_imsasound  # noqa: E402
import decode_kernel_opera  # noqa: E402
import decode_message_cel  # noqa: E402
import decode_rgb  # noqa: E402
import decode_txtr  # noqa: E402
import extract_symbols  # noqa: E402
import guest_ea  # noqa: E402
import m2_lzss  # noqa: E402


def form(chunks):
    body = b"".join(cid + struct.pack(">I", len(payload)) + payload
                    for cid, payload in chunks)
    return b"FORM" + struct.pack(">I", 4 + len(body)) + b"TXTR" + body


def test_lzss_literal_and_match():
    # 7 literals, then a 3-byte match from window offset 1.
    encoded = b"\xfe" + b"ABCDEFG" + b"\x00\x10"
    assert m2_lzss.decompress(encoded) == b"ABCDEFGBCD"
    assert m2_lzss.decompress(b"\xff" + b"12345678") == b"12345678"


def test_lzss_section_flag():
    assert m2_lzss.section_is_compressed(b"", 0x8)
    assert not m2_lzss.section_is_compressed(b"", 0x4)


def test_cbd2_decode_reset_and_delta():
    # 0x40 is even: reset predictor, then add 64^3/64 = 4096.
    assert decode_cbd2.decode_cbd2(b"\x40", 1) == [[4096]]
    assert decode_cbd2.decode_cbd2(b"\x40\x01", 1) == [[4096], [4096 + 0]]


def test_imsasound_tags_and_sqs2():
    tags = [(0x23, 2), (0x17, 1), (0x31, 16)]
    tags += [(0x01, 0)] * (decode_imsasound.HEADER_TAG_COUNT - 4)
    tags.append((0x00, 0))
    raw = b"".join(struct.pack(">II", tag, value) for tag, value in tags)
    parsed, offset = decode_imsasound.parse_tags(raw)
    assert offset == decode_imsasound.HEADER_SIZE
    assert decode_imsasound._fields(parsed)[0x23] == 2
    assert decode_imsasound.decode_sqs2(b"\x02\x03", 1) == [8, 10]


def test_message_cel_bitstream_and_chunks():
    assert decode_message_cel.Bitstream(b"\xa5").get(4) == 0xA
    chunks = decode_message_cel.parse_chunks(
        b"CCB " + struct.pack(">I", 12) + b"\0" * 4)
    assert chunks[b"CCB "][0] == 12
    assert decode_message_cel.rgb555(0x7C00) == (255, 0, 0)


def test_message_cel_packed_literal():
    # One 1-bpp literal row, width 1: offset word then literal packet.
    # 10-bit offset + 6 pad bits, packet 01, count 0, pixel 1, end.
    bits = "0000000000" "000000" "01" "000000" "1" "00"
    data = int(bits.ljust(32, "0"), 2).to_bytes(4, "big")
    rows = decode_message_cel.decode_packed(data, 1, 1, 1, 10, 6)
    assert rows == [[1]]


def test_rgb_decode(tmp_path):
    pytest.importorskip("PIL")
    source = tmp_path / "one.rgb"
    source.write_bytes(b"RAW RGB " + struct.pack(">HH", 1, 1) +
                       struct.pack(">H", 0xFFFF))
    output = tmp_path / "one.png"
    decode_rgb.decode_rgb(str(source), str(output))
    assert output.read_bytes().startswith(b"\x89PNG")


def test_txtr_form_chunks():
    form_type, chunks = decode_txtr.parse_chunks(form([(b"M2TX", b"abcd")]))
    assert form_type == b"TXTR"
    assert chunks[b"M2TX"] == b"abcd"


def _valid_txtr():
    """One 1x1 uncompressed indexed TXTR frame that decodes to a PNG."""
    m2tx = struct.pack(">IIHHHBB", 0, 0, 1, 1, 0x0408, 1, 0)
    m2td = struct.pack(">HHI", 1, 0, 8) + b"\x00" * 4
    m2pi = struct.pack(">II", 0, 0) + struct.pack(">I", 0x7F000000)
    return form([(b"M2TX", m2tx), (b"M2TD", m2td), (b"M2PI", m2pi)])


def _broken_txtr():
    """A valid FORM whose M2TX chunk is too short to describe a texture."""
    return form([(b"M2TX", b"short\0\0\0")])


def _cat_forms(*forms):
    """Bundle FORMs as a padded CAT, as used by multi-image .utf files."""
    body = b"".join(f + b"\0" * ((-len(f)) % 4) for f in forms)
    return b"CAT " + struct.pack(">I", 4 + len(body)) + b"TXTR" + body


def _two_lod_compressed_txtr():
    """Compressed 2-LOD TXTR whose LOD1 bitstream ends inside a texel."""
    m2tx = struct.pack(">IIHHHBB", 0, 1, 1, 1, 0x0408, 2, 0)
    m2ci = struct.pack(">4H4I", 0x0408, 0, 0, 0,
                       0x7F000000, 0, 0, 0)
    # LOD0: control 0x03 (one 4-pixel run) plus its 8-bit colour.
    # LOD1 starts at 15, leaving exactly one byte: a control byte that
    # requests a colour byte which is absent.  The trailing pad byte keeps
    # the M2TD payload 32-bit aligned for chunk parsing.
    m2td = struct.pack(">HHII", 2, 0, 12, 15) + b"\x03\x00\x00\x00"
    m2pi = struct.pack(">II", 0, 0) + struct.pack(">I", 0x7F000000)
    return form([(b"M2TX", m2tx), (b"M2TD", m2td), (b"M2CI", m2ci),
                 (b"M2PI", m2pi)])


def _decode_txtr_cli(*args):
    return subprocess.run(
        [sys.executable, os.path.join(TOOLS, "decode_txtr.py"), *args],
        capture_output=True, text=True, timeout=120)


def test_txtr_images_reports_failed_later_mip():
    tex = decode_txtr.Texture(_two_lod_compressed_txtr(), name="mip")
    errors = []
    images = tex.images(errors=errors)
    assert [suffix for suffix, _ in images] == ["_mip0"]
    assert len(errors) == 1
    assert errors[0].startswith("LOD1:")


def test_decode_txtr_direct_partial_bundle_is_nonzero(tmp_path):
    pytest.importorskip("PIL")
    mixed = tmp_path / "mixed.utf"
    mixed.write_bytes(_cat_forms(_valid_txtr(), _broken_txtr()))
    result = _decode_txtr_cli(str(mixed))
    assert result.returncode != 0, result.stdout + result.stderr
    assert "FAIL" in result.stdout
    assert "missing/short M2TX" in result.stdout
    png = tmp_path / "mixed_000.png"
    assert png.read_bytes().startswith(b"\x89PNG")
    assert not (tmp_path / "mixed_001.png").exists()


def test_decode_txtr_all_partial_bundle_is_nonzero(tmp_path):
    pytest.importorskip("PIL")
    root = tmp_path / "root"
    root.mkdir()
    (root / "mixed.utf").write_bytes(
        _cat_forms(_valid_txtr(), _broken_txtr()))
    out = tmp_path / "out"
    result = _decode_txtr_cli("--all", "--root", str(root),
                              "--outdir", str(out))
    assert result.returncode != 0, result.stdout + result.stderr
    assert "FAIL" in result.stdout
    assert "missing/short M2TX" in result.stdout
    assert "done ok 0 fail 1" in result.stdout
    png = out / "mixed.utf_000.png"
    assert png.read_bytes().startswith(b"\x89PNG")
    assert not (out / "mixed.utf_001.png").exists()


def test_decode_txtr_all_valid_multi_image_control(tmp_path):
    pytest.importorskip("PIL")
    root = tmp_path / "root"
    root.mkdir()
    (root / "ok.utf").write_bytes(_cat_forms(_valid_txtr(), _valid_txtr()))
    out = tmp_path / "out"
    result = _decode_txtr_cli("--all", "--root", str(root), "--outdir", str(out))
    assert result.returncode == 0, result.stdout + result.stderr
    assert "done ok 1 fail 0" in result.stdout
    for name in ("ok.utf_000.png", "ok.utf_001.png"):
        assert (out / name).read_bytes().startswith(b"\x89PNG")


def test_decode_txtr_all_failure_is_nonzero(tmp_path):
    root = tmp_path / "root"
    root.mkdir()
    (root / "broken.utf").write_bytes(_broken_txtr())
    result = _decode_txtr_cli("--all", "--root", str(root),
                              "--outdir", str(tmp_path / "out"))
    assert result.returncode != 0, result.stdout + result.stderr
    assert "missing/short M2TX" in result.stdout
    assert "done ok 0 fail 1" in result.stdout
    assert not (tmp_path / "out").exists()


def test_decode_txtr_all_valid_control_writes_png(tmp_path):
    pytest.importorskip("PIL")
    root = tmp_path / "root"
    root.mkdir()
    (root / "ok.utf").write_bytes(_valid_txtr())
    out = tmp_path / "out"
    result = _decode_txtr_cli("--all", "--root", str(root), "--outdir", str(out))
    assert result.returncode == 0, result.stdout + result.stderr
    png = out / "ok.utf.png"
    assert png.read_bytes().startswith(b"\x89PNG")


def test_decode_txtr_no_input_is_nonzero():
    result = _decode_txtr_cli()
    assert result.returncode == 1, result.stdout + result.stderr
    assert "usage:" in result.stdout


def test_decode_txtr_direct_failure_is_nonzero(tmp_path):
    broken = tmp_path / "broken.utf"
    broken.write_bytes(_broken_txtr())
    result = _decode_txtr_cli(str(broken))
    assert result.returncode != 0, result.stdout + result.stderr
    assert "missing/short M2TX" in result.stdout
    assert "no decoded output" in result.stdout
    assert not broken.with_suffix(".png").exists()


def test_extract_symbols_minimal_elf(tmp_path):
    elf = bytearray(128)
    elf[0:4] = b"\x7fELF"
    struct.pack_into(">I", elf, 0x20, 52)   # e_shoff
    struct.pack_into(">H", elf, 0x2E, 40)   # e_shentsize
    struct.pack_into(">H", elf, 0x30, 1)    # e_shnum
    struct.pack_into(">H", elf, 0x32, 0)    # e_shstrndx
    struct.pack_into(">IIIIIIIIII", elf, 52, 0, 0, 0, 0, 92, 1, 0, 0, 0, 0)
    path = tmp_path / "minimal.elf"
    path.write_bytes(elf)
    _, sections, names, symtab, _, _ = extract_symbols.parse_elf(str(path))
    assert len(sections) == 1
    assert names == [""]
    assert symtab is None


def _guest_image(tmp_path):
    generated = tmp_path / "generated"
    generated.mkdir()
    (generated / "game_data.c").write_text(
        "unsigned g_recomp_text_base = 0x100000;\n"
        "unsigned g_recomp_data_base = 0x200000;\n"
        "unsigned g_recomp_bss_base = 0x300000;\n"
        "unsigned g_recomp_text_size = 16;\n"
        "unsigned g_recomp_data_size = 4;\n"
        "unsigned g_recomp_bss_size = 8;\n"
        "unsigned g_recomp_data_file_offset = 16;\n")
    image = tmp_path / "game"
    image.write_bytes(b"\0" * 16 + b"\x01\x02\x03\x04")
    return generated, image


def test_guest_image_reads_data_extent(tmp_path):
    generated, image = _guest_image(tmp_path)
    img = guest_ea.Image(generated, image)
    assert img.data == b"\x01\x02\x03\x04"
    assert img.read(0x200000, 2) == b"\x01\x02"
    assert img.fixups == []


def test_guest_image_applies_data_fixups(tmp_path):
    generated = tmp_path / "generated"
    generated.mkdir()
    (generated / "game_data.c").write_text(
        "unsigned g_recomp_text_base = 0x100000;\n"
        "unsigned g_recomp_data_base = 0x200000;\n"
        "unsigned g_recomp_bss_base = 0x300000;\n"
        "unsigned g_recomp_text_size = 16;\n"
        "unsigned g_recomp_data_size = 8;\n"
        "unsigned g_recomp_bss_size = 8;\n"
        "unsigned g_recomp_data_file_offset = 16;\n"
        "unsigned g_recomp_data_fixup_count = 2;\n"
        "const uint32_t g_recomp_data_fixups[][2] = {\n"
        "  {0x0, 0x200100},\n"
        "  {0x4, 0x300010},\n"
        "  {0, 0}\n"
        "};\n")
    image = tmp_path / "game"
    image.write_bytes(b"\0" * 16 + struct.pack(">II", 0x4dc, 0x1234))
    img = guest_ea.Image(generated, image)
    # The trailing {0, 0} sentinel is not a fixup: offset 0 keeps 0x200100.
    assert img.fixups == [(0x0, 0x200100), (0x4, 0x300010)]
    assert img.data == struct.pack(">II", 0x200100, 0x300010)
    assert img.read(0x200000, 8) == struct.pack(">II", 0x200100, 0x300010)
    assert img.find32(0x300010) == [0x200004]


def test_guest_image_rejects_fixup_outside_data(tmp_path):
    layout = (
        "unsigned g_recomp_data_fixup_count = 1;\n"
        "const uint32_t g_recomp_data_fixups[][2] = {\n"
        "  {0x2, 0x300010},\n"
        "  {0, 0}\n"
        "};\n")
    generated = tmp_path / "generated"
    generated.mkdir()
    (generated / "game_data.c").write_text(
        "unsigned g_recomp_data_size = 4;\n"
        "unsigned g_recomp_data_file_offset = 16;\n" + layout)
    image = tmp_path / "game"
    image.write_bytes(b"\0" * 16 + b"\x01\x02\x03\x04")
    with pytest.raises(SystemExit, match="outside the 4-byte .data extent"):
        guest_ea.Image(generated, image)


def test_guest_ea_arithmetic():
    assert guest_ea.sign16(0xFFFF) == -1
    assert guest_ea.resolve(24, -14856) == 0x17C5F8
    assert guest_ea.resolve_addend("data", 0x6588) == guest_ea.DATA_BASE + 0x6588


def test_kernel_opera_helpers():
    assert decode_kernel_opera.rev16(b"\x01\x02\x03\x04") == b"\x02\x01\x04\x03"
    assert decode_kernel_opera.rev32(b"\x01\x02\x03\x04") == b"\x04\x03\x02\x01"
    assert decode_kernel_opera.entropy(b"") == 0.0
    assert decode_kernel_opera.entropy(b"aaaa") == 0.0


def test_message_cel_chunk_walk_and_malformed():
    good = (b"ABCD" + struct.pack(">I", 12) + b"payl" +
            b"EFGH" + struct.pack(">I", 8))
    assert decode_message_cel.parse_chunks(good) == {
        b"ABCD": (12, b"payl"), b"EFGH": (8, b"")}
    # A zero-size chunk used to leave the offset unchanged and hang.
    with pytest.raises(ValueError, match="invalid chunk"):
        decode_message_cel.parse_chunks(b"ABCD" + struct.pack(">I", 0))
    # A chunk that claims more bytes than the buffer has.
    with pytest.raises(ValueError, match="invalid chunk"):
        decode_message_cel.parse_chunks(b"ABCD" + struct.pack(">I", 999))


def test_kernel_opera_feed_decompress_literal_and_eos():
    # CTRL byte 0x80 selects a literal; the following bytes terminate in M1.
    assert decode_kernel_opera.feed_decompress(b"\x80A\x00\x00") == b"A"
    assert decode_kernel_opera.feed_decompress(b"") == b""
    # The cap is a soft bound checked between match runs: the decoder stops
    # near it instead of expanding the whole input.
    capped = decode_kernel_opera.feed_decompress(b"\x80A" * 100, cap=7)
    assert 7 <= len(capped) <= 30


def test_cbd2_fast_paths_match_scalar():
    np = pytest.importorskip("numpy")
    rng = np.random.default_rng(1234)
    for channels in (1, 2, 3):
        raw = bytes(int(v) for v in rng.integers(0, 256, 257 * channels))
        scalar = [v for frame in decode_cbd2.decode_cbd2(raw, channels)
                  for v in frame]
        fast = [int(v) for v in decode_cbd2.decode_cbd2_fast(raw, channels)]
        streamed = []
        for block in decode_cbd2.iter_decode_cbd2(raw, channels,
                                                  block_frames=13):
            streamed.extend(np.frombuffer(block, dtype="<i2").tolist())
        assert scalar == fast
        assert scalar == streamed


def test_guest_ea_check_flags_live_use_only(tmp_path):
    from types import SimpleNamespace

    trap = guest_ea.KNOWN_TRAPS[0]
    live = tmp_path / "live.c"
    live.write_text(f"x = 0x{trap['wrong']:X};\n")
    comment = tmp_path / "comment.c"
    comment.write_text(f"/* old addend 0x{trap['wrong']:X} */\n")
    assert guest_ea.cmd_check(SimpleNamespace(files=[live], json=False)) == 1
    assert guest_ea.cmd_check(SimpleNamespace(files=[comment], json=False)) == 0
