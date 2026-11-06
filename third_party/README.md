# Third-party code

## stb_vorbis

- Files: `stb_vorbis.c`, `stb_vorbis.h`
- Version: 1.22
- Upstream: https://github.com/nothings/stb
- License: public domain / MIT dual license; full text in
  `stb_vorbis_LICENSE.txt` and repeated at the end of `stb_vorbis.c`

The `.h` file is a declarations-only wrapper used by the Ogg stream decoder.
`stb_vorbis.c` is compiled as its own translation unit by CMake.

## License texts

License texts for dependencies that are linked but not vendored are kept under
`licenses/`:

- `licenses/LGPL-2.1.txt` for FFmpeg (`libavcodec`, `libavutil`)
- `licenses/wxWindows-3.1.txt` for wxWidgets

See the top-level `THIRD_PARTY_NOTICES` for the component list and notices.
