#pragma once

class wxFrame;

// Apply the IMSA plaque as the frame's icon set (title bar, taskbar, switcher).
// Windows uses the .ico embedded in the executable; macOS and Linux scale the
// runtime imsa-wcr-m2-recomp-256.png found beside the launcher, in the .app's
// Resources, or under share/icons/hicolor. Returns false when none is found.
bool apply_launcher_icon(wxFrame& frame);

#if defined(__APPLE__)
extern "C" void m2_launcher_set_app_icon(const char* utf8_path);
#endif
