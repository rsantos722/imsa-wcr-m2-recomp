// utf8_path.h — open runtime-owned files by UTF-8 path on every platform.
//
// The runtime image loaders receive the game root as UTF-8: the host resolves
// --game-data and converts it with m2host::platform::path_to_utf8 before
// calling m2_recomp_set_game_root. Narrow fopen() on Windows resolves those
// bytes against the ANSI code page, so a game installed under a non-ASCII
// path (for example C:\Users\Jörg\Games\IMSA) fails to load. This mirrors the
// policy in host/core/platform.cpp — convert once to UTF-16 and call
// _wfopen — but stays in C so the standalone runtime and its tests do not
// depend on the C++ host layer.
#pragma once

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// fopen() with a UTF-8 path. `mode` is the ordinary CRT mode string.
// Returns NULL on failure, exactly like fopen().
FILE* m2_fopen_utf8(const char* path, const char* mode);

#ifdef __cplusplus
}
#endif
