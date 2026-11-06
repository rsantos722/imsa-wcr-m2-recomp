#include "ui_text.h"

#include <clocale>
#include <cstdio>

int main() {
    // Exercise the same conversion as UI labels under a non-UTF-8 locale.
    std::setlocale(LC_ALL, "C");
    const auto choose = launcher_ui::wx_text("Choose ISO\xe2\x80\xa6");
    const auto resolution = launcher_ui::wx_text("640\xc3\x97" "480");
    const auto info = launcher_ui::wx_text("Info \xe2\x93\x98");
    if (choose.length() != 11 || choose.Last().GetValue() != 0x2026 ||
        resolution.length() != 7 || resolution[3].GetValue() != 0x00d7 ||
        info.length() != 6 || info.Last().GetValue() != 0x24d8) {
        std::fputs("UTF-8 UI labels depend on the system locale\n", stderr);
        return 1;
    }
    return 0;
}
