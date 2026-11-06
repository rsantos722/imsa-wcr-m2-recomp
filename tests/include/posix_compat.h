/* posix_compat.h — POSIX calls test executables use that MinGW-w64
 * does not provide (setenv/unsetenv/mkdtemp). Test-only. */
#pragma once

#include <stdlib.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <string.h>

static inline int m2_compat_setenv(const char* name, const char* value, int overwrite) {
    if (!overwrite && getenv(name) != NULL) return 0;
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
static inline int m2_compat_unsetenv(const char* name) {
    return _putenv_s(name, "") == 0 ? 0 : -1;
}
static inline char* m2_compat_mkdtemp(char* tmpl) {
    if (_mktemp_s(tmpl, strlen(tmpl) + 1) != 0) return NULL;
    return _mkdir(tmpl) == 0 ? tmpl : NULL;
}
#define setenv m2_compat_setenv
#define unsetenv m2_compat_unsetenv
#define mkdtemp m2_compat_mkdtemp
#endif
