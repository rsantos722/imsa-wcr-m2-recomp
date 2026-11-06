#!/usr/bin/env python3
"""Compile the verified disc extent/hash manifest for the native ISO importer."""
import json
import pathlib
import sys

source, output = map(pathlib.Path, sys.argv[1:])
manifest = json.loads(source.read_text(encoding='utf-8'))
rows = []
for entry in manifest['files']:
    if entry['dir']:
        continue
    path = pathlib.PurePosixPath(entry['path'])
    assert not path.is_absolute() and '..' not in path.parts and '\\' not in str(path)
    digest = entry['sha256']
    assert len(digest) == 64 and all(c in '0123456789abcdef' for c in digest)
    offset = int(entry['lbas'][0], 16) * manifest['block_size']
    rows.append(f'    {{{json.dumps(str(path))}, {offset}ULL, {entry["size"]}ULL, "{digest}"}},')
output.write_text('// Generated from launcher/disc-manifest.json; do not edit.\n'
                  'static constexpr DiscFile disc_files[] = {\n' + '\n'.join(rows) + '\n};\n',
                  encoding='utf-8')
