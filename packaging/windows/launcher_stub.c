/* launcher_stub.c - the program at the top of the Windows package.
 *
 * Starts bin\m2launcher.exe from the folder holding this program and passes
 * its command-line arguments along. Linked statically so it needs no DLLs. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

#define PATH_CHARS 32768

static wchar_t exe[PATH_CHARS];
static wchar_t cmd[PATH_CHARS];

/* The part of the command line after this program's own name. */
static const wchar_t *arguments_after_program(const wchar_t *line) {
    if (*line == L'"') {
        line++;
        while (*line && *line != L'"') line++;
        if (*line) line++;
    } else {
        while (*line && *line != L' ' && *line != L'\t') line++;
    }
    while (*line == L' ' || *line == L'\t') line++;
    return line;
}

static int fail(const wchar_t *message) {
    MessageBoxW(NULL, message, L"IMSA WCR M2 Recomp", MB_OK | MB_ICONERROR);
    return 1;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR ignored, int show) {
    static const wchar_t relative[] = L"bin\\m2launcher.exe";
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    const wchar_t *args = arguments_after_program(GetCommandLineW());
    DWORD length = GetModuleFileNameW(NULL, exe, PATH_CHARS);
    wchar_t *slash;
    (void)instance; (void)previous; (void)ignored; (void)show;

    if (length == 0 || length >= PATH_CHARS) return fail(L"Cannot locate this program's folder.");
    slash = wcsrchr(exe, L'\\');
    if (!slash) return fail(L"Cannot locate this program's folder.");
    slash[1] = L'\0';
    if (wcslen(exe) + wcslen(relative) >= PATH_CHARS) return fail(L"The folder path is too long.");
    wcscat(exe, relative);

    /* Two quotes and a separating space, plus the terminator. */
    if (wcslen(exe) + wcslen(args) + 4 > PATH_CHARS) return fail(L"The command line is too long.");
    cmd[0] = L'"';
    wcscpy(cmd + 1, exe);
    wcscat(cmd, L"\" ");
    wcscat(cmd, args);

    startup.cb = sizeof(startup);
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        return fail(L"Cannot start bin\\m2launcher.exe. Make sure the whole package was extracted.");
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}
