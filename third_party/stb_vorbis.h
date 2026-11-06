// stb_vorbis.h — declarations-only include of the third-party decoder.
//
// third_party/stb_vorbis.c is the upstream single-file distribution (v1.22,
// public domain). It is compiled standalone as its own TU by CMake; every
// other translation unit includes this wrapper, which defines
// STB_VORBIS_HEADER_ONLY so only the API section is pulled in.
#ifndef M2_VENDOR_STB_VORBIS_H
#define M2_VENDOR_STB_VORBIS_H

#ifndef STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_HEADER_ONLY
#endif
#include "stb_vorbis.c"

#endif  // M2_VENDOR_STB_VORBIS_H
