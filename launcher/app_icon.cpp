#include "app_icon.h"

#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/iconbndl.h>
#include <wx/image.h>
#include <wx/stdpaths.h>
#include <wx/wx.h>

#include <initializer_list>

namespace {

wxString executable_dir() {
    return wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath();
}

wxString join(std::initializer_list<wxString> parts) {
    wxString path;
    for (const wxString& part : parts) {
        if (!path.empty()) path += wxFILE_SEP_PATH;
        path += part;
    }
    return path;
}

// The window-manager sizes a title bar, taskbar, Alt-Tab switcher and HiDPI
// variants pick from; every one is derived from the 256px runtime PNG.
constexpr int kBundleSizes[] = {16, 24, 32, 48, 64, 128, 256};

wxString find_runtime_png() {
    const wxString dir = executable_dir();
    const wxString candidates[] = {
        // Build tree and Windows/Linux install (bin/).
        join({dir, "imsa-wcr-m2-recomp-256.png"}),
        // macOS bundle: Contents/MacOS/../Resources.
        join({dir, "..", "Resources", "imsa-wcr-m2-recomp-256.png"}),
        // Linux install: bin/../share/icons/hicolor.
        join({dir, "..", "share", "icons", "hicolor", "256x256", "apps",
              "imsa-wcr-m2-recomp.png"}),
    };
    for (const wxString& path : candidates)
        if (wxFileExists(path)) return path;
    return {};
}

} // namespace

bool apply_launcher_icon(wxFrame& frame) {
#if defined(__WXMSW__)
    // The .ico embedded by m2launcher.rc already carries hand-sized 16..256
    // images, and Explorer shows the same resource for the .exe.
    wxIconBundle embedded(wxS("IDI_ICON1"), 0);
    if (embedded.IsOk() && !embedded.IsEmpty()) {
        frame.SetIcons(embedded);
        return true;
    }
#endif

    static bool png_handler_added = false;
    if (!png_handler_added) {
        wxImage::AddHandler(new wxPNGHandler);
        png_handler_added = true;
    }

    const wxString path = find_runtime_png();
    if (path.empty()) return false;
    const wxImage master(path, wxBITMAP_TYPE_PNG);
    if (!master.IsOk()) return false;

    wxIconBundle bundle;
    for (int size : kBundleSizes) {
        const wxImage image = size == master.GetWidth()
                                  ? master
                                  : master.Scale(size, size, wxIMAGE_QUALITY_HIGH);
        wxIcon icon;
        icon.CopyFromBitmap(wxBitmap(image));
        if (icon.IsOk()) bundle.AddIcon(icon);
    }
    if (bundle.IsEmpty()) return false;
    frame.SetIcons(bundle);
#if defined(__APPLE__)
    // Only matters for the bare Mach-O; the .app's Dock tile comes from
    // CFBundleIconFile.
    const wxScopedCharBuffer utf8 = path.utf8_str();
    m2_launcher_set_app_icon(utf8.data());
#endif
    return true;
}
