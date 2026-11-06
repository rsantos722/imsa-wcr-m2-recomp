// utf8_path.c — see utf8_path.h.
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
// mingw-w64 marks _wfopen deprecated in favour of _wfopen_s unless this is
// set. The runtime wants the plain FILE* form used by the host platform
// layer, so suppress the security-warning annotation.
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "utf8_path.h"

#include <stdlib.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wchar.h>

// Convert a NUL-terminated UTF-8 string to a freshly allocated UTF-16 one.
// If the bytes are not valid UTF-8, fall back to the ANSI code page so a path
// supplied by the narrow CRT (for example an environment variable) keeps the
// meaning it had before this wrapper existed. Caller frees the result.
static wchar_t* utf8_to_wide(const char* text) {
    if (!text) return NULL;

    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int length = MultiByteToWideChar(code_page, flags, text, -1, NULL, 0);
    if (length <= 0) {
        code_page = CP_ACP;
        flags = 0;
        length = MultiByteToWideChar(code_page, flags, text, -1, NULL, 0);
    }
    if (length <= 0) return NULL;

    wchar_t* out = (wchar_t*)malloc((size_t)length * sizeof(*out));
    if (!out) return NULL;
    if (MultiByteToWideChar(code_page, flags, text, -1, out, length) <= 0) {
        free(out);
        return NULL;
    }
    return out;
}
#endif

FILE* m2_fopen_utf8(const char* path, const char* mode) {
#if defined(_WIN32)
    wchar_t* wide_path = utf8_to_wide(path);
    wchar_t* wide_mode = utf8_to_wide(mode);
    FILE* file = NULL;
    if (wide_path && wide_mode) file = _wfopen(wide_path, wide_mode);
    free(wide_path);
    free(wide_mode);
    return file;
#else
    return fopen(path, mode);
#endif
}
