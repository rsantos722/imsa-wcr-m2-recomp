#pragma once

// Arguments handed to the driving game's main().
//
// LaunchMe.m2 starts driving/game with an empty argv, so that is the default.
// The only source of guest arguments is M2_GUEST_ARGS: the host's own command
// line must never reach the guest. The game's Parse_Args treats any argument
// starting with '-', '/' or '\' as a flag keyed on its second character, so a
// host path such as /Users/... would act as the developer flag -u.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Splits `guest_args` (the M2_GUEST_ARGS value, may be NULL) on spaces and
// tabs into `vec`, with "imsa" as argv[0]. `buf` receives the token storage.
// Returns the argument count: 0 when `guest_args` is NULL or holds no tokens.
// At most `cap` entries are written; input longer than `buf` is truncated.
static inline int m2_guest_boot_args(const char* guest_args, char* buf,
                                     size_t buf_size, char** vec, int cap) {
    if (!guest_args || !buf || buf_size == 0 || !vec || cap < 2) return 0;
    size_t len = 0;
    while (guest_args[len] != '\0' && len + 1 < buf_size) {
        buf[len] = guest_args[len];
        ++len;
    }
    buf[len] = '\0';

    static char program_name[] = "imsa";
    int n = 0;
    vec[n++] = program_name;
    char* p = buf;
    while (*p != '\0' && n < cap) {
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '\0') break;
        vec[n++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') ++p;
        if (*p != '\0') *p++ = '\0';  // end this token at its separator
    }
    return n > 1 ? n : 0;
}

#ifdef __cplusplus
}
#endif
