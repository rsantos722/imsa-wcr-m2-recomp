#pragma once

#include <string>
#include <wx/string.h>

namespace launcher_ui {
// Source literals and launcher messages are UTF-8. The default wxString
// narrow-string conversion uses the system locale, which varies on Windows.
inline wxString wx_text(const std::string& text) {
    return wxString::FromUTF8(text);
}
}
