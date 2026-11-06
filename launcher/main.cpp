// Host-only desktop launcher built from the platform's own controls through
// wxWidgets (Cocoa on macOS, Win32 on Windows, GTK on Linux). Layout uses
// stock widgets and system fonts/colours; nothing is custom-styled.
//
// SDL is initialised without video: it delivers joystick events for binding
// capture and launches m2host. Keyboard and mouse input arrive as native
// events and are translated into the same SDL events HostControls stores.
#include <SDL3/SDL.h>
#include <wx/wx.h>
#include <wx/dataview.h>
#include <wx/notebook.h>
#include <wx/scrolwin.h>
#include <wx/spinctrl.h>
#include <wx/statline.h>
#include <wx/tglbtn.h>
#include "app_icon.h"
#include "asset_model.h"
#include "asset_viewport.h"
#include "asset_raster.h"
#include "native_keys.h"
#include "ui_text.h"
#include "run_flags.h"
#include "disc_import.h"
#include "host_path.h"
#include "save_data.h"
#include "host_controls.h"
#include "platform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <future>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using m2host::platform::path_from_utf8;
using m2host::platform::path_to_utf8;
using namespace m2host::input;
using namespace launcher_flags;
using launcher_assets::AssetChoice;
using launcher_assets::Model;
using launcher_assets::Transform;
using launcher_assets::Vec3;

namespace {

using launcher_ui::wx_text;
std::string utf8(const wxString& s) { return s.ToStdString(wxConvUTF8); }

SDL_Process* launch(const Settings& s, const fs::path& profile, std::string& message,
                    bool shooter2d, const fs::path& saves) {
    std::error_code ec;
    const fs::path host = path_from_utf8(s.executable.data());
    if (!fs::is_regular_file(host, ec)) {
        message = "Cannot find " + path_to_utf8(host) +
                  ". Please ensure no files are missing.";
        return nullptr;
    }
    if (!fs::is_directory(launch_data_root(s, shooter2d), ec)) {
        message = "Complete game setup first.";
        return nullptr;
    }

    std::vector<std::string> argv = launch_arguments(s, profile, shooter2d);
    SDL_Environment* environment = SDL_CreateEnvironment(true);
    if (!environment) {
        message = SDL_GetError();
        return nullptr;
    }
    for (const auto& f : s.flags) {
        if (!is_launcher_flag(f.name.data())) continue;
        if (should_export_flag(f.enabled, f.value.data()))
            SDL_SetEnvironmentVariable(environment, f.name.data(), f.value.data(), true);
        else
            SDL_UnsetEnvironmentVariable(environment, f.name.data());
    }
    if (!apply_save_option(saves, environment) ||
        !apply_boot_option(shooter2d, environment)) {
        message = SDL_GetError();
        SDL_DestroyEnvironment(environment);
        return nullptr;
    }
    std::vector<const char*> args;
    for (const auto& a : argv) args.push_back(a.c_str());
    args.push_back(nullptr);
    const auto props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args.data());
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, environment);
    SDL_Process* process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    SDL_DestroyEnvironment(environment);
    message = process ? "Game running."
                      : std::string("Launch failed: ") + SDL_GetError();
    return process;
}

// True when `host` links the recompiled game. A stub host (built without game
// data) starts a window that never draws, so the launcher prefers any host
// that answers `--has-guest` with success. Results are cached per path.
bool host_has_guest(const fs::path& host) {
    static std::map<std::string, bool> cache;
    const std::string key = path_to_utf8(host);
    if (const auto it = cache.find(key); it != cache.end()) return it->second;
    bool ok = false;
    const std::string arg = "--has-guest";
    const char* args[] = {key.c_str(), arg.c_str(), nullptr};
    const auto props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_Process* process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (process) {
        int exit_code = -1;
        ok = SDL_WaitProcess(process, true, &exit_code) && exit_code == 0;
        SDL_DestroyProcess(process);
    }
    cache[key] = ok;
    return ok;
}

fs::path executable_path(const char* base) {
    return launcher_host::find_host_executable(path_from_utf8(base ? base : "."),
                                               host_has_guest);
}

// Screenshot destination shown in the Controls tooltip. M2_SCREENSHOT_ROOT
// wins; otherwise follow the selected host executable, as save management
// does, because the launcher and host can live in sibling build directories.
fs::path screenshot_directory(const Settings& settings) {
    fs::path directory = m2host::platform::get_env_path("M2_SCREENSHOT_ROOT");
    if (directory.empty())
        directory = m2host::platform::default_screenshot_dir_for(
            path_from_utf8(settings.executable.data()));
    return fs::absolute(directory);
}

// ---------------------------------------------------------------------------
// Car Viewer
//
// The launcher decodes the original disc MEMIMAGE car models and their texture
// bundles itself (see asset_model.h and asset_texture.h for the format
// evidence) and depth-tests them into an RGBA image. The viewer never
// launches, patches, or reads from the running game.
constexpr float kDefaultYaw = 0.62f, kDefaultPitch = -0.26f;

launcher_assets::PreviewRaster g_preview;
bool g_preview_valid = false;

struct AssetViewer {
    bool race_models = true, loaded_race_models = false;
    int body_choice = 0, variant_choice = 0, lod_choice = 0;
    int chassis_choice = 0;  // showroom model index; in-race pack is fixed (cars1)
    int tire_choice = 0;     // showroom tire brand index (Firestone default)
    int part_choice = -1;    // -1 = the model's own default chain
    bool wireframe = false;
    bool preview_lighting = false;
    float yaw = kDefaultYaw, pitch = kDefaultPitch, zoom = 1.0f;
    Model chassis, tire;
    int loaded_chassis = -2, loaded_tire = -2;
    std::string loaded_root;
    std::string message;
};
AssetViewer g_asset_viewer;

struct ViewportScene {
    const launcher_assets::Part* chassis = nullptr;
    const launcher_assets::Part* tire = nullptr;
    std::array<Transform, 4> tire_mounts{};
    int tire_count = 0;
    float yaw = 0.0f, pitch = 0.0f, zoom = 1.0f;
    bool wireframe = false;
    float width = 0.0f, height = 0.0f;  // panel size in logical points
    bool preview_lighting = false;
};
ViewportScene g_rendered_viewport;

bool viewport_box(const ViewportScene& scene, Vec3& lo, Vec3& hi) {
    lo = {1e30f, 1e30f, 1e30f};
    hi = {-1e30f, -1e30f, -1e30f};
    bool any = false;
    auto expand = [&](const launcher_assets::Part* part, const Transform& transform) {
        if (!part || part->mesh.positions.empty()) return;
        const auto& mesh = part->mesh;
        for (int bits = 0; bits < 8; ++bits) {
            const Vec3 p = transform.point({bits & 1 ? mesh.bbox_max.x : mesh.bbox_min.x,
                                           bits & 2 ? mesh.bbox_max.y : mesh.bbox_min.y,
                                           bits & 4 ? mesh.bbox_max.z : mesh.bbox_min.z});
            lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y); lo.z = std::min(lo.z, p.z);
            hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y); hi.z = std::max(hi.z, p.z);
        }
        any = true;
    };
    expand(scene.chassis, Transform{});
    for (int i = 0; i < scene.tire_count; ++i) expand(scene.tire, scene.tire_mounts[i]);
    return any;
}

// Renders `scene` into g_preview at `pixel_scale` physical pixels per point,
// capped at a 2048-pixel longest edge. The image is reused until the model,
// camera, panel size or preview options change. Returns false when there is
// nothing to draw.
bool render_viewport_scene(const ViewportScene& scene, float pixel_scale) {
    Vec3 lo, hi;
    if (scene.width < 1.0f || scene.height < 1.0f || !viewport_box(scene, lo, hi)) {
        g_preview_valid = false;
        return false;
    }
    const auto camera = launcher_assets::fit_camera(lo, hi, scene.width, scene.height, scene.zoom);
    const float scale = std::min(pixel_scale, 2048.0f / std::max(scene.width, scene.height));
    const int width = std::clamp(int(std::ceil(scene.width * scale)), 1, 2048);
    const int height = std::clamp(int(std::ceil(scene.height * scale)), 1, 2048);
    const auto& previous = g_rendered_viewport;
    bool changed = !g_preview_valid || width != g_preview.width || height != g_preview.height ||
        scene.chassis != previous.chassis || scene.tire != previous.tire ||
        scene.tire_count != previous.tire_count || scene.yaw != previous.yaw ||
        scene.pitch != previous.pitch || scene.zoom != previous.zoom ||
        scene.width != previous.width || scene.height != previous.height ||
        scene.wireframe != previous.wireframe || scene.preview_lighting != previous.preview_lighting;
    for (int i = 0; i < scene.tire_count; ++i)
        changed |= scene.tire_mounts[i].m != previous.tire_mounts[i].m;
    if (!changed) return true;
    g_preview.reset(width, height);
    auto raster_camera = camera;
    raster_camera.cx *= width / scene.width;
    raster_camera.cy *= height / scene.height;
    raster_camera.focal *= std::min(width / scene.width, height / scene.height);
    launcher_assets::PreviewInstance instances[5];
    size_t count = 0;
    if (scene.chassis) instances[count++] = {scene.chassis, {}};
    for (int i = 0; i < scene.tire_count; ++i)
        if (scene.tire) instances[count++] = {scene.tire, scene.tire_mounts[i]};
    launcher_assets::render_preview(g_preview, raster_camera, instances, count,
                                    scene.yaw, scene.pitch, scene.preview_lighting,
                                    scene.wireframe);
    g_rendered_viewport = scene;
    g_preview_valid = true;
    return true;
}

void viewer_sync_models(AssetViewer& viewer, const std::string& root) {
    const auto store = [](Model& target, Model&& value) {
        target = std::move(value);
        g_preview_valid = false;
    };
    // Loads `choice` into `target`, or empties it and reports the error.
    const auto load = [&](Model& target, const AssetChoice& choice) {
        Model loaded;
        std::string error;
        const bool ok = launcher_assets::load_model(path_from_utf8(root) / choice.relative_path,
                                                    choice.label, loaded, error);
        store(target, ok ? std::move(loaded) : Model{});
        if (!ok) viewer.message = std::string(choice.label) + ": " + error;
        return ok;
    };
    if (viewer.loaded_root != root) {
        viewer.loaded_root = root;
        viewer.loaded_chassis = viewer.loaded_tire = -2;
    }
    if (root.empty()) {
        store(viewer.chassis, Model{});
        store(viewer.tire, Model{});
        viewer.loaded_chassis = -2;
        viewer.loaded_tire = -2;
        viewer.message = "Complete game setup first.";
        return;
    }
    const int effective_chassis = viewer.race_models ? 0 : viewer.chassis_choice;
    if (viewer.loaded_chassis != effective_chassis ||
        viewer.loaded_race_models != viewer.race_models) {
        const AssetChoice& choice =
            viewer.race_models
                ? launcher_assets::kRaceCarChoices[effective_chassis]
                : launcher_assets::kShowroomChassisChoices[effective_chassis];
        if (load(viewer.chassis, choice)) {
            viewer.part_choice = -1;
            viewer.message = "Loaded " + std::string(choice.label) +
                             " (" + std::to_string(viewer.chassis.parts.size()) +
                             " model parts)";
        }
        viewer.loaded_chassis = effective_chassis;
        viewer.loaded_race_models = viewer.race_models;
    }
    if (viewer.loaded_tire != viewer.tire_choice) {
        load(viewer.tire, launcher_assets::kShowroomTireChoices[viewer.tire_choice]);
        viewer.loaded_tire = viewer.tire_choice;
    }
}

int viewer_part_index(const Model& model, int part_choice) {
    if (model.parts.empty()) return -1;
    const int index = (part_choice >= 0 && size_t(part_choice) < model.parts.size())
                          ? part_choice
                          : model.default_part;
    return (index >= 0 && size_t(index) < model.parts.size()) ? index : -1;
}

const launcher_assets::Part* viewer_part(const Model& model, int part_choice) {
    const int index = viewer_part_index(model, part_choice);
    return index < 0 ? nullptr : &model.parts[size_t(index)];
}

// The interactive scene: the chosen body plus the four wheels it mounts.
ViewportScene viewer_scene(const AssetViewer& viewer, float width, float height) {
    const auto* shown_part = viewer_part(viewer.chassis, viewer.part_choice);
    const auto* tire_part = viewer.race_models ? nullptr : viewer.tire.parts.empty()
                                ? nullptr
                                : &viewer.tire.parts[size_t(std::max(0, viewer.tire.default_part))];
    if (viewer.race_models) {
        for (const auto& part : viewer.chassis.parts)
            if (part.component == "wheel" &&
                (part.lod <= viewer.lod_choice || !tire_part))
                tire_part = &part;
    }
    std::array<Transform, 4> tire_mounts{};
    int tire_count = 0;
    if (tire_part && shown_part) {
        const int mount_index =
            viewer.race_models
                ? ((shown_part->component == "body" && shown_part->body >= 0 &&
                    shown_part->body < 4)
                       ? shown_part->body
                       : viewer.body_choice)
                : viewer.chassis_choice;
        tire_count = launcher_assets::wheel_placements(viewer.chassis, mount_index, tire_mounts);
        if (tire_count != 4) tire_part = nullptr;
    }
    return {shown_part, tire_part, tire_mounts, tire_count, viewer.yaw, viewer.pitch,
            viewer.zoom, viewer.wireframe, width, height, viewer.preview_lighting};
}

// Offline stills use the same raster path as the interactive tab.
int preview_asset(const fs::path& input, const fs::path& output, int part_index,
                  float yaw, float pitch, const fs::path& tire_path) {
    Model model;
    std::string error;
    if (!launcher_assets::load_model(input, input.stem().string(), model, error)) {
        SDL_Log("%s", error.c_str());
        return 1;
    }
    const auto* part = viewer_part(model, part_index);
    if (part_index >= 0 && size_t(part_index) >= model.parts.size()) {
        SDL_Log("Asset part %d does not exist", part_index);
        return 2;
    }
    if (!part) return 1;
    std::array<Transform, 4> still_mounts{};
    int still_tire_count = 0;
    const launcher_assets::Part* still_tire = nullptr;
    if (model.race_pack && part->component == "body") {
        for (const auto& candidate : model.parts)
            if (candidate.component == "wheel" &&
                (candidate.lod <= part->lod || !still_tire)) still_tire = &candidate;
        if (still_tire) {
            still_tire_count = launcher_assets::wheel_placements(model, part->body, still_mounts);
            if (still_tire_count != 4) still_tire = nullptr;
        }
    }
    Model still_tire_model;
    if (!tire_path.empty() && !model.race_pack) {
        if (launcher_assets::load_model(tire_path, tire_path.stem().string(),
                                        still_tire_model, error)) {
            still_tire = viewer_part(still_tire_model, -1);
            if (still_tire) {
                still_tire_count = launcher_assets::wheel_placements(model, -1, still_mounts);
                if (still_tire_count != 4) still_tire = nullptr;
            }
        } else {
            SDL_Log("%s", error.c_str());
            return 1;
        }
    }
    const ViewportScene scene{part, still_tire, still_mounts, still_tire_count, yaw, pitch,
                              1.1f, false, 1024, 768};
    if (!render_viewport_scene(scene, 1.0f)) return 1;
    SDL_Surface* pixels = SDL_CreateSurfaceFrom(g_preview.width, g_preview.height,
                                                SDL_PIXELFORMAT_RGBA32, g_preview.rgba.data(),
                                                g_preview.width * 4);
    const bool saved = pixels && SDL_SaveBMP(pixels, path_to_utf8(output).c_str());
    if (!saved) SDL_Log("Asset still: %s", SDL_GetError());
    SDL_DestroySurface(pixels);
    return saved ? 0 : 1;
}

// A notebook tab that scrolls vertically once its content is taller than the
// window (large system fonts, small windows). While everything fits no
// scrollbar shows and it lays out exactly like a plain panel. Width always
// tracks the window, as a plain panel's does, so wide content never forces
// sideways scrolling or a wider window. After changing what a page shows,
// call FitInside() on it rather than Layout() so the scroll range follows.
class TabPage final : public wxScrolledWindow {
public:
    explicit TabPage(wxWindow* parent)
        : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxScrolledWindowStyle | wxTAB_TRAVERSAL) {
        SetScrollRate(0, FromDIP(10));
    }

    wxSize GetBestVirtualSize() const override {
        wxSize size = wxScrolledWindow::GetBestVirtualSize();
        size.x = GetClientSize().x;
        return size;
    }
};

// Paints the depth-tested preview at the display's pixel density. Drag to
// orbit, scroll to zoom.
class CarViewport final : public wxWindow {
public:
    explicit CarViewport(wxWindow* parent)
        : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(FromDIP(wxSize(320, 240)));
        Bind(wxEVT_PAINT, &CarViewport::paint, this);
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
            last_ = e.GetPosition();
            dragging_ = true;
            if (!HasCapture()) CaptureMouse();
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { release(); });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { dragging_ = false; });
        Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
            if (!dragging_) return;
            const wxPoint delta = e.GetPosition() - last_;
            last_ = e.GetPosition();
            auto& viewer = g_asset_viewer;
            viewer.yaw += delta.x * 0.012f;
            viewer.pitch = std::clamp(viewer.pitch + delta.y * 0.012f, -1.45f, 1.45f);
            Refresh();
        });
        Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& e) {
            if (e.GetWheelAxis() != wxMOUSE_WHEEL_VERTICAL || !e.GetWheelDelta()) return;
            const float steps = float(e.GetWheelRotation()) / float(e.GetWheelDelta());
            auto& viewer = g_asset_viewer;
            viewer.zoom = std::clamp(viewer.zoom * std::pow(1.15f, steps), 0.25f, 20.0f);
            Refresh();
        });
    }

private:
    bool dragging_ = false;
    wxPoint last_;

    void release() {
        dragging_ = false;
        if (HasCapture()) ReleaseMouse();
    }

    void paint(wxPaintEvent&) {
        wxPaintDC dc(this);
        const wxSize size = GetClientSize();
        const double scale = GetContentScaleFactor();
        const auto scene = viewer_scene(g_asset_viewer, float(size.x), float(size.y));
        if (!render_viewport_scene(scene, float(scale))) {
            dc.SetBackground(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW)));
            dc.Clear();
            return;
        }
        wxImage image(g_preview.width, g_preview.height, false);
        unsigned char* rgb = image.GetData();
        const auto& rgba = g_preview.rgba;
        for (size_t i = 0, n = size_t(g_preview.width) * g_preview.height; i < n; ++i) {
            rgb[i * 3 + 0] = rgba[i * 4 + 0];
            rgb[i * 3 + 1] = rgba[i * 4 + 1];
            rgb[i * 3 + 2] = rgba[i * 4 + 2];
        }
        // The raster may be capped below display density; scale it to fill.
        const double image_scale = double(g_preview.width) / std::max(1, size.x);
        if (std::abs(image_scale - scale) >= 1e-3)
            image.Rescale(std::max(1, int(std::lround(size.x * scale))),
                          std::max(1, int(std::lround(size.y * scale))), wxIMAGE_QUALITY_BILINEAR);
        dc.DrawBitmap(wxBitmap(image, -1, scale), 0, 0);
    }
};

class LauncherFrame;
LauncherFrame* g_frame = nullptr;

// The binding-capture prompt. Keys, controller inputs and clicks or scrolls
// inside the dialog are captured by LauncherFrame; Escape or Cancel closes it.
class CaptureDialog final : public wxDialog {
public:
    CaptureDialog(wxWindow* parent, const wxString& action)
        : wxDialog(parent, wxID_ANY, "Bind " + action) {
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        auto* prompt = new wxStaticText(this, wxID_ANY,
            "Press a key or button, or move an analog axis.");
        sizer->Add(prompt, 0, wxALL, FromDIP(12));
        sizer->Add(CreateStdDialogButtonSizer(wxCANCEL), 0, wxALL | wxEXPAND, FromDIP(12));
        SetSizerAndFit(sizer);
        CentreOnParent();
    }
};

// Labelled form rows: right-aligned labels in the first column.
wxFlexGridSizer* form_grid() {
    auto* grid = new wxFlexGridSizer(2, wxSize(12, 8));
    grid->AddGrowableCol(1, 1);
    return grid;
}

// `control` is a wxWindow or a wxSizer.
template <typename Control>
void add_form_row(wxFlexGridSizer* grid, wxWindow* parent, const wxString& label, Control* control) {
    grid->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL);
    grid->Add(control, 0, wxALIGN_CENTER_VERTICAL);
}

class LauncherFrame final : public wxFrame {
public:
    LauncherFrame(const fs::path& settings_dir, const std::string& initial_tab)
        : wxFrame(nullptr, wxID_ANY, "IMSA WCR M2 Recomp Launcher"),
          settings_dir_(settings_dir),
          profile_(settings_dir / "controls.cfg"),
          settings_path_(settings_dir / "launcher.cfg"),
          game_root_(launcher_disc::install_root(settings_dir)),
          backups_root_(settings_dir / "save-backups"),
          timer_(this) {
        load_state();
        // Follow the host launch_game() will run, not the launcher's own
        // directory, so save management and gameplay share one card in CMake's
        // split build tree.
        saves_root_ = launcher_saves::save_root(path_from_utf8(settings_.executable.data()));
        build_ui();
        load_settings_into_controls();
        refresh_saves();
        refresh_controls_list();
        refresh_state();
        refresh_viewer();
        // wxOSX keeps the first page's contents hidden behind a later page
        // when the selection is set before the frame is first shown.
        CallAfter([this, initial_tab] { select_tab(initial_tab); });

        Bind(wxEVT_CLOSE_WINDOW, &LauncherFrame::on_close, this);
        Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& e) {
            if (!e.GetActive()) release_inputs();
            e.Skip();
        });
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); });
        timer_.Start(16);
    }

    // Native keyboard and mouse events, seen application-wide before any
    // control handles them. Returns true when the event was consumed.
    bool filter_input(wxEvent& event) {
        const wxEventType type = event.GetEventType();
        if (type == wxEVT_CHAR_HOOK || type == wxEVT_KEY_DOWN || type == wxEVT_KEY_UP)
            return key_event(static_cast<wxKeyEvent&>(event));
        if (event.IsKindOf(wxCLASSINFO(wxMouseEvent)))
            return mouse_event(static_cast<wxMouseEvent&>(event));
        return false;
    }

private:
    fs::path settings_dir_, profile_, settings_path_, game_root_, saves_root_, backups_root_;
    Settings settings_;
    bool settings_valid_ = true, controls_valid_ = true, game_ready_ = false;
    std::string message_ = "Ready.";
    wxString shown_message_;
    HostControls controls_;
    launcher_saves::Summary save_summary_;
    std::vector<fs::path> save_backups_;
    fs::path selected_backup_;
    launcher_disc::Progress import_progress_;
    std::future<launcher_disc::Result> importing_;
    SDL_Process* process_ = nullptr;
    wxTimer timer_;

    // Binding capture.
    CaptureDialog* capture_dialog_ = nullptr;
    Uint64 capture_after_ = 0;
    std::optional<HostBinding> captured_;
    std::set<int> held_keys_;
    std::optional<wxPoint> last_mouse_;

    // Window contents.
    wxNotebook* notebook_ = nullptr;
    std::vector<std::pair<std::string, wxWindow*>> pages_;
    wxButton* play_button_ = nullptr;
    wxButton* shooter_button_ = nullptr;
    wxButton* reset_invalid_button_ = nullptr;

    wxStaticText* setup_status_ = nullptr;
    wxStaticText* setup_prompt_ = nullptr;
    wxButton* choose_iso_ = nullptr;
    wxGauge* import_gauge_ = nullptr;
    wxButton* cancel_import_ = nullptr;
    wxStaticText* install_label_ = nullptr;
    wxTextCtrl* install_path_ = nullptr;

    wxChoice* resolution_ = nullptr;
    wxCheckBox* fullscreen_ = nullptr;
    wxCheckBox* borderless_ = nullptr;

    // Controls bound one-to-one to a launcher flag; see flag_checkbox() and
    // flag_choice(). A choice row maps either integer `levels` or string
    // `strings` (exactly one is set); index 0 always leaves the flag unset.
    struct FlagChoice {
        wxChoice* choice;
        const char* flag;
        const int* levels;
        const char* const* strings;
        const char* const* names;
        int count;
    };
    std::vector<std::pair<const char*, wxCheckBox*>> flag_boxes_;
    std::vector<FlagChoice> flag_choices_;
    // Texture filtering drives M2_TEXTURE_FILTER and M2_ANISOTROPY together.
    wxChoice* texture_filtering_ = nullptr;

    wxDataViewListCtrl* actions_list_ = nullptr;
    std::vector<Action> row_actions_;
    std::vector<int> shown_levels_;
    wxStaticBoxSizer* selected_box_ = nullptr;
    wxButton* bind_button_ = nullptr;
    wxButton* add_binding_ = nullptr;
    wxButton* clear_binding_ = nullptr;
    wxStaticText* action_help_ = nullptr;
    wxString screenshot_help_text_;
    wxSpinCtrlDouble* deadzone_ = nullptr;
    wxStaticText* devices_ = nullptr;
    std::array<wxToggleButton*, 2> controller_type_{};
    wxSizer* controls_type_row_ = nullptr;
    wxSizer* controller_type_group_ = nullptr;
    wxSizer* patched_preview_group_ = nullptr;
    int controls_visibility_mode_ = -1;
    wxNotebook* controls_modes_ = nullptr;
    std::array<wxToggleButton*, 3> patched_preview_{};
    int patched_preview_selection_ = 1;
    bool adding_binding_ = false;

    wxStaticText* save_summary_text_ = nullptr;
    wxButton* create_backup_ = nullptr;
    wxListBox* backups_list_ = nullptr;
    wxButton* restore_backup_ = nullptr;
    wxButton* reset_saves_ = nullptr;

    wxChoice* viewer_mode_ = nullptr;
    wxChoice* viewer_car_ = nullptr;
    wxChoice* viewer_paint_ = nullptr;
    wxChoice* viewer_lod_ = nullptr;
    wxChoice* viewer_chassis_ = nullptr;
    wxChoice* viewer_tire_ = nullptr;
    wxFlexGridSizer* viewer_grid_ = nullptr;
    std::vector<wxWindow*> race_rows_, showroom_rows_;
    wxCheckBox* viewer_wireframe_ = nullptr;
    wxCheckBox* viewer_lighting_ = nullptr;
    CarViewport* viewport_ = nullptr;
    wxStaticText* viewer_stats_ = nullptr;

    // ---- State ------------------------------------------------------------

    // The host and data paths always follow this install, whatever was saved.
    void apply_install_paths() {
        copy(settings_.executable, path_to_utf8(executable_path(SDL_GetBasePath())));
        copy(settings_.data, path_to_utf8(game_root_ / "driving/data"));
    }

    void load_state() {
        settings_valid_ = !fs::exists(settings_path_) || load_settings(settings_path_, settings_, message_);
        settings_.use_host_defaults = false;
        settings_.fast_forward = false;
        apply_install_paths();
        game_ready_ = launcher_disc::installed(game_root_);
        if (!game_ready_ && settings_valid_) message_ = "Select an IMSA Racing ISO image to install.";
        ensure_launcher_flags(settings_);
        controls_.defaults();
        controls_.open_devices();
        controls_valid_ = !fs::exists(profile_) || controls_.load(profile_, message_);
    }

    fs::path viewer_data_root() const {
        std::error_code ec;
        const fs::path candidate = path_from_utf8(settings_.data.data());
        return (game_ready_ || fs::is_directory(candidate, ec)) ? candidate : fs::path{};
    }

    // ---- Layout -----------------------------------------------------------

    wxStaticText* hint(wxWindow* parent, const wxString& text, int wrap_dip = 520) {
        auto* label = new wxStaticText(parent, wxID_ANY, text);
        label->SetWindowVariant(wxWINDOW_VARIANT_SMALL);
        label->Wrap(FromDIP(wrap_dip));
        return label;
    }

    wxCheckBox* flag_checkbox(wxWindow* parent, const wxString& label, const char* flag) {
        auto* box = new wxCheckBox(parent, wxID_ANY, label);
        box->Bind(wxEVT_CHECKBOX, [this, flag](wxCommandEvent& e) {
            set_option_on(settings_, flag, e.IsChecked());
            auto_save_settings();
        });
        flag_boxes_.emplace_back(flag, box);
        return box;
    }

    wxChoice* flag_choice(wxWindow* parent, const char* flag, const int* levels,
                          const char* const* strings, const char* const* names, int count) {
        auto* choice = new wxChoice(parent, wxID_ANY);
        const FlagChoice row{choice, flag, levels, strings, names, count};
        choice->Bind(wxEVT_CHOICE, [this, row](wxCommandEvent&) {
            const int index = row.choice->GetSelection();
            const int flag_row = find_flag(settings_, row.flag);
            if (flag_row < 0 || index < 0 || index >= row.count) return;
            auto& f = settings_.flags[size_t(flag_row)];
            copy(f.value, row.levels ? std::to_string(row.levels[index]) : std::string(row.strings[index]));
            f.enabled = index > 0;
            auto_save_settings();
        });
        flag_choices_.push_back(row);
        return choice;
    }
    wxChoice* flag_choice(wxWindow* parent, const char* flag, const int* levels,
                          const char* const* names, int count) {
        return flag_choice(parent, flag, levels, nullptr, names, count);
    }
    wxChoice* flag_choice(wxWindow* parent, const char* flag, const char* const* strings,
                          const char* const* names, int count) {
        return flag_choice(parent, flag, nullptr, strings, names, count);
    }

    // A row of mutually exclusive toggle buttons; `on_select` runs after the
    // clicked one is shown pressed.
    template <size_t N>
    void add_toggle_group(wxWindow* parent, wxSizer* group, std::array<wxToggleButton*, N>& buttons,
                          const std::array<wxString, N>& labels, int selected,
                          std::function<void(int)> on_select) {
        for (int i = 0; i < int(N); ++i) {
            auto* btn = new wxToggleButton(parent, wxID_ANY, labels[size_t(i)],
                                           wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
            btn->SetValue(i == selected);
            btn->Bind(wxEVT_TOGGLEBUTTON, [&buttons, i, on_select](wxCommandEvent&) {
                for (int j = 0; j < int(N); ++j) buttons[size_t(j)]->SetValue(j == i);
                on_select(i);
            });
            buttons[size_t(i)] = btn;
            group->Add(btn, 0, wxALIGN_CENTER_VERTICAL | (i > 0 ? wxLEFT : 0), FromDIP(i > 0 ? 4 : 0));
        }
    }

    void build_ui() {
        auto* root = new wxPanel(this);
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        notebook_ = new wxNotebook(root, wxID_ANY);
        add_page("game", "Setup", build_setup_page(notebook_));
        add_page("graphics", "Graphics", build_graphics_page(notebook_));
        add_page("controls", "Controls", build_controls_page(notebook_));
        add_page("enhancements", "Enhancements/Hacks", build_enhancements_page(notebook_));
        add_page("saves", "Save Data", build_saves_page(notebook_));
        add_page("viewer", "Car Viewer", build_viewer_page(notebook_));
        notebook_->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, [this](wxBookCtrlEvent& e) {
            if (current_tab() == "saves") refresh_saves();
            if (current_tab() == "viewer") refresh_viewer();
            e.Skip();
        });
        sizer->Add(notebook_, 1, wxEXPAND | wxALL, FromDIP(10));

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        reset_invalid_button_ = new wxButton(root, wxID_ANY, "Reset Unreadable Settings");
        reset_invalid_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { reset_invalid_settings(); });
        buttons->Add(reset_invalid_button_, 0, wxALIGN_CENTER_VERTICAL);
        buttons->AddStretchSpacer();
        shooter_button_ = new wxButton(root, wxID_ANY, "Play shooter2d");
        shooter_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { launch_game(true); });
        play_button_ = new wxButton(root, wxID_ANY, "Play IMSA Racing");
        play_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { launch_game(false); });
        play_button_->SetDefault();
        buttons->Add(shooter_button_, 0, wxRIGHT, FromDIP(8));
        buttons->Add(play_button_, 0);
        sizer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
        root->SetSizer(sizer);

        auto* status = CreateStatusBar(2);
        const wxString version = "v" IMSA_VERSION;
        const int widths[] = {-1, status->GetTextExtent(version).GetWidth() + FromDIP(24)};
        status->SetStatusWidths(2, widths);
        status->SetStatusText(version, 1);
        auto* frame_sizer = new wxBoxSizer(wxVERTICAL);
        frame_sizer->Add(root, 1, wxEXPAND);
        SetSizer(frame_sizer);
        SetMinSize(FromDIP(wxSize(760, 560)));
        SetSize(FromDIP(wxSize(900, 680)));
        CentreOnScreen();
    }

    void add_page(const char* id, const char* title, wxWindow* page) {
        notebook_->AddPage(page, title);
        pages_.emplace_back(id, page);
    }

    std::string current_tab() const {
        const int index = notebook_->GetSelection();
        return index >= 0 && size_t(index) < pages_.size() ? pages_[size_t(index)].first : "";
    }

    void select_tab(const std::string& id) {
        for (size_t i = 0; i < pages_.size(); ++i) {
            if (pages_[i].first != id) continue;
            // Re-selecting the current page is a no-op, so pass through another one.
            if (notebook_->GetSelection() == int(i)) notebook_->ChangeSelection(i ? 0 : 1);
            notebook_->SetSelection(i);
        }
    }

    wxWindow* build_setup_page(wxWindow* parent) {
        auto* page = new TabPage(parent);
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        auto* box = new wxStaticBoxSizer(wxVERTICAL, page, "Installation");
        wxWindow* body = box->GetStaticBox();
        setup_status_ = new wxStaticText(body, wxID_ANY, "");
        box->Add(setup_status_, 0, wxALL, FromDIP(8));
        setup_prompt_ = new wxStaticText(body, wxID_ANY,
            "Select an IMSA Racing ISO image to install.");
        setup_prompt_->Wrap(FromDIP(560));
        box->Add(setup_prompt_, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        choose_iso_ = new wxButton(body, wxID_ANY, wx_text("Choose ISO…"));
        choose_iso_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { choose_iso(); });
        box->Add(choose_iso_, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        auto* progress = new wxBoxSizer(wxHORIZONTAL);
        import_gauge_ = new wxGauge(body, wxID_ANY, 100);
        cancel_import_ = new wxButton(body, wxID_ANY, "Cancel");
        cancel_import_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { import_progress_.cancel = true; });
        progress->Add(import_gauge_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        progress->Add(cancel_import_, 0, wxALIGN_CENTER_VERTICAL);
        box->Add(progress, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        auto* location = new wxBoxSizer(wxHORIZONTAL);
        install_label_ = new wxStaticText(body, wxID_ANY, "Location:");
        install_path_ = new wxTextCtrl(body, wxID_ANY, wx_text(path_to_utf8(game_root_)),
                                       wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
        location->Add(install_label_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        location->Add(install_path_, 1, wxALIGN_CENTER_VERTICAL);
        box->Add(location, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        sizer->Add(box, 0, wxEXPAND | wxALL, FromDIP(12));
        page->SetSizer(sizer);
        return page;
    }

    wxWindow* build_graphics_page(wxWindow* parent) {
        auto* page = new TabPage(parent);
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        auto* display = new wxStaticBoxSizer(wxVERTICAL, page, "Display");
        auto* display_grid = form_grid();
        resolution_ = new wxChoice(display->GetStaticBox(), wxID_ANY);
        resolution_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            const int index = resolution_->GetSelection();
            if (index >= 0 && index < kResolutionPresetCount) {
                settings_.width = kResolutionPresets[index].width;
                settings_.height = kResolutionPresets[index].height;
                auto_save_settings();
            }
        });
        fullscreen_ = new wxCheckBox(display->GetStaticBox(), wxID_ANY, "Full screen");
        fullscreen_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& e) {
            settings_.fullscreen = e.IsChecked();
            update_display_controls();
            auto_save_settings();
        });
        auto* size_row = new wxBoxSizer(wxHORIZONTAL);
        size_row->Add(resolution_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(16));
        size_row->Add(fullscreen_, 0, wxALIGN_CENTER_VERTICAL);
        add_form_row(display_grid, display->GetStaticBox(), "Window size:", size_row);
        if (kBorderlessFullscreenSupported) {
            borderless_ = new wxCheckBox(display->GetStaticBox(), wxID_ANY, "Borderless");
            borderless_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& e) {
                settings_.borderless = e.IsChecked();
                auto_save_settings();
            });
            display_grid->AddSpacer(0);
            display_grid->Add(borderless_, 0, wxALIGN_CENTER_VERTICAL);
        }
        display->Add(display_grid, 0, wxEXPAND | wxALL, FromDIP(8));
        display->Add(flag_checkbox(display->GetStaticBox(), "Disable Overscan", "M2_CROP_OVERSCAN"),
                     0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        sizer->Add(display, 0, wxEXPAND | wxALL, FromDIP(12));

        auto* rendering = new wxStaticBoxSizer(wxVERTICAL, page, "Rendering");
        auto* grid = form_grid();
        wxWindow* body = rendering->GetStaticBox();
        add_form_row(grid, body, "Internal resolution:",
                     flag_choice(body, "M2_RENDER_SCALE", kRenderScaleValues, kRenderScaleNames, kRenderScaleCount));
        texture_filtering_ = new wxChoice(body, wxID_ANY);
        texture_filtering_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            const int index = texture_filtering_->GetSelection();
            if (index < 0 || index >= kTextureFilteringChoiceCount) return;
            set_texture_filtering(settings_, kTextureFilteringChoices[index]);
            auto_save_settings();
        });
        add_form_row(grid, body, "Texture filtering:", texture_filtering_);
        add_form_row(grid, body, "Menu and video scaling:",
                     flag_choice(body, "M2_BITMAP_FILTER", kBitmapFilterValues, kBitmapFilterNames, kBitmapFilterCount));
        add_form_row(grid, body, "Anti-aliasing (MSAA):",
                     flag_choice(body, "M2_ANTIALIASING", kAntialiasingLevelValues, kAntialiasingLevelNames, kAntialiasingLevelCount));
        for (const auto& [label, flag] : {std::pair{"Enable dithering", "M2_RGB555_DITHER"},
                                          std::pair{"High-resolution effects", "M2_HIRES_EFFECTS"},
                                          std::pair{"Disable car LODs", "M2_DISABLE_CAR_LOD"}}) {
            grid->AddSpacer(0);
            grid->Add(flag_checkbox(body, label, flag), 0, wxALIGN_CENTER_VERTICAL);
        }
        rendering->Add(grid, 0, wxEXPAND | wxALL, FromDIP(8));
        sizer->Add(rendering, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

        page->SetSizer(sizer);
        return page;
    }

    wxWindow* build_controls_page(wxWindow* parent) {
        auto* page = new TabPage(parent);
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        controls_modes_ = new wxNotebook(page, wxID_ANY);
        // Tabs follow kControlsTabModes, not the saved m2_controls_mode values.
        const char* names[] = {"Custom (Recommended)", "Original", "Patched"};
        const char* descriptions[] = {
            "Binds inputs directly to in-game actions. In-game controller settings are ignored.",
            "Binds inputs to M2 controller buttons. As in the original game, the in-game controller layout settings have no effect.",
            "Fixes the in-game controller settings so all layouts work. Choose your layout in the in-game Options menu.",
        };
        for (int i = 0; i < 3; ++i) {
            auto* tab = new wxPanel(controls_modes_);
            auto* content = new wxBoxSizer(wxVERTICAL);
            content->Add(hint(tab, wx_text(descriptions[i]), 660), 0, wxALL, FromDIP(8));
            tab->SetSizer(content);
            controls_modes_->AddPage(tab, names[i]);
        }
        controls_modes_->ChangeSelection(controls_tab(settings_.controls_mode));
        controls_modes_->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, [this](wxBookCtrlEvent& e) {
            settings_.controls_mode = kControlsTabModes[size_t(controls_modes_->GetSelection())];
            controls_.reset();
            rebuild_controls_rows();
            auto_save_settings();
            e.Skip();
        });
        sizer->Add(controls_modes_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

        auto* steering = flag_checkbox(page, "Improved Analog Steering", "M2_IMPROVED_ANALOG_STEERING");
        steering->SetToolTip(
            "Completely overhauled analog controls, making steering much more predictable.");
        sizer->Add(steering, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

        auto* type_row = new wxBoxSizer(wxHORIZONTAL);
        controls_type_row_ = type_row;
        controller_type_group_ = new wxBoxSizer(wxHORIZONTAL);
        patched_preview_group_ = new wxBoxSizer(wxHORIZONTAL);
        controller_type_group_->Add(new wxStaticText(page, wxID_ANY, "Controller type:"), 0,
                      wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        add_toggle_group<2>(page, controller_type_group_, controller_type_,
            {controller_type_label(ControllerType::Pad), controller_type_label(ControllerType::Joystick)},
            settings_.controller_type == ControllerType::Joystick ? 1 : 0, [this](int i) {
                settings_.controller_type = i == 1 ? ControllerType::Joystick : ControllerType::Pad;
                auto_save_settings();
                refresh_controls_list();
            });
        patched_preview_group_->Add(new wxStaticText(page, wxID_ANY, "Preview Patched layout:"), 0,
                      wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
        add_toggle_group<3>(page, patched_preview_group_, patched_preview_,
            {"1: Wheel", "2: Digital pad", "3: Analog stick"}, patched_preview_selection_, [this](int i) {
                patched_preview_selection_ = i;
                refresh_controls_list();
            });
        type_row->Add(controller_type_group_, 0, wxALIGN_CENTER_VERTICAL);
        type_row->Add(patched_preview_group_, 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(type_row, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

        actions_list_ = new wxDataViewListCtrl(page, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                               wxDV_SINGLE | wxDV_ROW_LINES | wxDV_VERT_RULES);
        // Size the fixed-text columns from their longest entry, whatever the font.
        int label_width = 0, effect_width = 0, shooter_width = 0;
        const auto fit = [this](int& width, const char* text) {
            width = std::max(width, actions_list_->GetTextExtent(wx_text(text)).x + FromDIP(16));
        };
        fit(label_width, "Control / action");
        fit(effect_width, "IMSA Action");
        fit(shooter_width, "Shooter2d Action");
        for (const auto& section : kActionRowSections) {
            for (size_t r = 0; r < section.count; ++r) {
                const Action action = section.rows[r];
                fit(label_width, action_row_label(action));
                if (action >= Action::DriveLeft && action != Action::Quit) continue;
                fit(shooter_width, shooter_action_effect(action));
                for (const ControllerType type : {ControllerType::Pad, ControllerType::Joystick})
                    fit(effect_width, action_effect(action, M2_CONTROLS_ORIGINAL, type));
                for (int layout = 0; layout < 3; ++layout)
                    fit(effect_width, action_effect(action, M2_CONTROLS_PATCHED, ControllerType::Pad, layout));
            }
        }
        // The list gives up height down to this floor before its page scrolls.
        actions_list_->SetMinSize(wxSize(-1, FromDIP(140)));
        actions_list_->AppendTextColumn("Control / action", wxDATAVIEW_CELL_INERT, label_width);
        actions_list_->AppendTextColumn("Binding(s)", wxDATAVIEW_CELL_INERT, FromDIP(190));
        actions_list_->AppendTextColumn("IMSA Action", wxDATAVIEW_CELL_INERT, effect_width);
        actions_list_->AppendTextColumn("Shooter2d Action", wxDATAVIEW_CELL_INERT, shooter_width);
        actions_list_->AppendProgressColumn("Input", wxDATAVIEW_CELL_INERT, FromDIP(45));
        actions_list_->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [this](wxDataViewEvent&) { refresh_binding_controls(); });
        actions_list_->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED, [this](wxDataViewEvent&) { start_capture(); });
        actions_list_->Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
            wxDataViewItem item;
            wxDataViewColumn* column = nullptr;
            actions_list_->HitTest(event.GetPosition(), item, column);
            const int row = item.IsOk() ? int(actions_list_->ItemToRow(item)) : -1;
            const wxString tip = row >= 0 && size_t(row) < row_actions_.size()
                                     ? control_tooltip(row_actions_[row]) : wxString();
            if (actions_list_->GetToolTipText() != tip) {
                if (tip.empty()) actions_list_->UnsetToolTip();
                else actions_list_->SetToolTip(tip);
            }
            event.Skip();
        });
        sizer->Add(actions_list_, 1, wxEXPAND | wxALL, FromDIP(12));

        selected_box_ = new wxStaticBoxSizer(wxHORIZONTAL, page, "Selected action");
        wxWindow* box = selected_box_->GetStaticBox();
        bind_button_ = new wxButton(box, wxID_ANY, wx_text("Bind…"));
        bind_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_capture(); });
        add_binding_ = new wxButton(box, wxID_ANY, wx_text("Add…"));
        add_binding_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_capture(true); });
        clear_binding_ = new wxButton(box, wxID_ANY, "Clear");
        clear_binding_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { clear_selected_binding(); });
        selected_box_->Add(bind_button_, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(6));
        selected_box_->Add(add_binding_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        selected_box_->Add(clear_binding_, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(6));
        // rebuild_controls_rows() below sets its label and tooltip.
        action_help_ = new wxStaticText(box, wxID_ANY, wx_text("Screenshot info ⓘ"));
        screenshot_help_text_ = wx_text(screenshot_tooltip(screenshot_directory(settings_)));
        selected_box_->Add(action_help_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
        selected_box_->AddStretchSpacer();
        selected_box_->Add(new wxStaticText(box, wxID_ANY, "Axis dead zone:"), 0,
                           wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        deadzone_ = new wxSpinCtrlDouble(box, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                         wxSP_ARROW_KEYS, 0.0, 0.5, 0.08, 0.05);
        deadzone_->SetDigits(2);
        deadzone_->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent& e) { set_deadzone(float(e.GetValue())); });
        selected_box_->Add(deadzone_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        sizer->Add(selected_box_, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

        auto* footer = new wxBoxSizer(wxHORIZONTAL);
        devices_ = new wxStaticText(page, wxID_ANY, "");
        footer->Add(devices_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        auto* defaults = new wxButton(page, wxID_ANY, "Reset to Keyboard Defaults");
        defaults->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (settings_.controls_mode == M2_CONTROLS_CUSTOM) controls_.custom_defaults();
            else {
                std::vector<HostControl> custom;
                for (const auto& c : controls_.bindings)
                    if (c.action >= Action::DriveLeft) custom.push_back(c);
                controls_.defaults();
                controls_.bindings.erase(std::remove_if(controls_.bindings.begin(), controls_.bindings.end(),
                    [](const HostControl& c) { return c.action >= Action::DriveLeft; }), controls_.bindings.end());
                controls_.bindings.insert(controls_.bindings.end(), custom.begin(), custom.end());
                controls_.reset();
            }
            controls_valid_ = true;
            auto_save_controls();
            message_ = "Controls reset to keyboard defaults.";
            refresh_controls_list();
            refresh_state();
        });
        footer->Add(defaults, 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(footer, 0, wxEXPAND | wxALL, FromDIP(12));
        page->SetSizer(sizer);
        rebuild_controls_rows();
        return page;
    }

    wxWindow* build_enhancements_page(wxWindow* parent) {
        auto* page = new TabPage(parent);
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        for (const auto& section : kEnhancementSections) {
            const bool last = &section == &kEnhancementSections[std::size(kEnhancementSections) - 1];
            auto* group = new wxStaticBoxSizer(wxVERTICAL, page, section.heading);
            wxWindow* body = group->GetStaticBox();
            for (size_t i = 0; i < section.count; ++i) {
                const auto& option = section.options[i];
                group->AddSpacer(FromDIP(i == 0 ? 4 : 6));
                group->Add(flag_checkbox(body, option.label, option.flag), 0, wxLEFT | wxRIGHT, FromDIP(8));
                group->Add(hint(body, option.description, 660), 0, wxLEFT | wxRIGHT, FromDIP(30));
            }
            group->AddSpacer(FromDIP(6));
            if (last) {
                // The livery picker closes the final (Hacks) section.
                auto* livery_row = new wxBoxSizer(wxHORIZONTAL);
                livery_row->Add(new wxStaticText(body, wxID_ANY, kCarVariantOptionLabel), 0,
                                wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
                livery_row->Add(flag_choice(body, "M2_PLAYER_CAR_VARIANT", kCarVariantValues,
                                            kCarVariantNames, kCarVariantCount),
                                0, wxALIGN_CENTER_VERTICAL);
                group->Add(livery_row, 0, wxLEFT | wxRIGHT, FromDIP(8));
                group->AddSpacer(FromDIP(2));
                group->Add(hint(body, kCarVariantOptionDescription, 660),
                           0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
            }
            sizer->AddSpacer(FromDIP(8));
            sizer->Add(group, 0, wxEXPAND | wxLEFT | wxRIGHT | (last ? wxBOTTOM : 0), FromDIP(12));
        }
        page->SetSizer(sizer);
        return page;
    }

    wxWindow* build_saves_page(wxWindow* parent) {
        auto* page = new TabPage(parent);
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        auto* current = new wxStaticBoxSizer(wxVERTICAL, page, "Current Saves");
        wxWindow* body = current->GetStaticBox();
        save_summary_text_ = new wxStaticText(body, wxID_ANY, wx_text("Checking save data…"));
        current->Add(save_summary_text_, 0, wxALL, FromDIP(8));
        current->Add(hint(body, "Close the game before restoring or resetting save files."),
                     0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        auto* actions = new wxBoxSizer(wxHORIZONTAL);
        auto* open_saves = new wxButton(body, wxID_ANY, "Open Save Folder");
        open_saves->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { open_folder(saves_root_); });
        auto* refresh = new wxButton(body, wxID_ANY, "Refresh");
        refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { refresh_saves(); });
        create_backup_ = new wxButton(body, wxID_ANY, "Create Backup");
        create_backup_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            auto result = launcher_saves::backup(saves_root_, backups_root_);
            message_ = result.message;
            if (result.ok) selected_backup_ = result.backup;
            refresh_saves();
        });
        actions->Add(open_saves, 0, wxRIGHT, FromDIP(8));
        actions->Add(refresh, 0, wxRIGHT, FromDIP(8));
        actions->Add(create_backup_, 0);
        current->Add(actions, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        sizer->Add(current, 0, wxEXPAND | wxALL, FromDIP(12));

        auto* history = new wxStaticBoxSizer(wxVERTICAL, page, "Backups");
        body = history->GetStaticBox();
        history->Add(hint(body, "Restoring a backup also backs up your current saves."),
                     0, wxALL, FromDIP(8));
        backups_list_ = new wxListBox(body, wxID_ANY);
        // Its best height grows to ten rows; let it shrink before the page scrolls.
        backups_list_->SetMinSize(wxSize(-1, FromDIP(80)));
        backups_list_->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) {
            const int index = backups_list_->GetSelection();
            if (index >= 0 && size_t(index) < save_backups_.size()) selected_backup_ = save_backups_[size_t(index)];
            refresh_state();
        });
        history->Add(backups_list_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        auto* backup_actions = new wxBoxSizer(wxHORIZONTAL);
        restore_backup_ = new wxButton(body, wxID_ANY, wx_text("Restore Selected Backup…"));
        restore_backup_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { confirm_restore(); });
        auto* open_backups = new wxButton(body, wxID_ANY, "Open Backup Folder");
        open_backups->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { open_folder(backups_root_); });
        backup_actions->Add(restore_backup_, 0, wxRIGHT, FromDIP(8));
        backup_actions->Add(open_backups, 0);
        history->Add(backup_actions, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        sizer->Add(history, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

        auto* reset = new wxStaticBoxSizer(wxVERTICAL, page, "Reset");
        body = reset->GetStaticBox();
        reset->Add(new wxStaticText(body, wxID_ANY,
            "Clears save data for both IMSA and shooter2d.\n"
            "Your current save is backed up first."), 0, wxALL, FromDIP(8));
        reset_saves_ = new wxButton(body, wxID_ANY, wx_text("Reset Save Data…"));
        reset_saves_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { confirm_reset(); });
        reset->Add(reset_saves_, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        sizer->Add(reset, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

        page->SetSizer(sizer);
        return page;
    }

    wxChoice* viewer_choice(wxWindow* parent, const wxArrayString& items, int selected, int AssetViewer::*field) {
        auto* choice = new wxChoice(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, items);
        choice->SetSelection(selected);
        choice->Bind(wxEVT_CHOICE, [this, choice, field](wxCommandEvent&) {
            g_asset_viewer.*field = choice->GetSelection();
            refresh_viewer();
        });
        return choice;
    }

    void viewer_row(wxWindow* parent, const wxString& label, wxWindow* control, std::vector<wxWindow*>& group) {
        auto* text = new wxStaticText(parent, wxID_ANY, label);
        viewer_grid_->Add(text, 0, wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL);
        viewer_grid_->Add(control, 0, wxALIGN_CENTER_VERTICAL);
        group.push_back(text);
        group.push_back(control);
    }

    wxWindow* build_viewer_page(wxWindow* parent) {
        auto* page = new TabPage(parent);
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        auto* top = new wxBoxSizer(wxHORIZONTAL);

        // Two label/choice pairs per row; In-Race and Showroom show their own pairs.
        viewer_grid_ = new wxFlexGridSizer(4, wxSize(8, 6));
        AssetViewer& viewer = g_asset_viewer;
        std::vector<wxWindow*> always;
        wxArrayString modes;
        modes.Add("Showroom");
        modes.Add("In-Race");
        viewer_mode_ = new wxChoice(page, wxID_ANY, wxDefaultPosition, wxDefaultSize, modes);
        viewer_mode_->SetSelection(viewer.race_models ? 1 : 0);
        viewer_mode_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            g_asset_viewer.race_models = viewer_mode_->GetSelection() == 1;
            refresh_viewer();
        });
        viewer_row(page, "Category:", viewer_mode_, always);

        wxArrayString cars, paints, lods, chassis, tires;
        for (const char* car : {"Ferrari", "Riley & Scott", "Kudzu", "Courage"}) cars.Add(car);
        for (const char* paint : {"Default", "Alt 1", "Alt 2"}) paints.Add(paint);
        for (const char* lod : {"High", "Medium", "Low"}) lods.Add(lod);
        for (int i = 0; i < launcher_assets::kShowroomChassisChoiceCount; ++i)
            chassis.Add(wx_text(launcher_assets::kShowroomChassisChoices[i].label));
        for (int i = 0; i < launcher_assets::kShowroomTireChoiceCount; ++i)
            tires.Add(wx_text(launcher_assets::kShowroomTireChoices[i].label));
        viewer_car_ = viewer_choice(page, cars, viewer.body_choice, &AssetViewer::body_choice);
        viewer_row(page, "Car:", viewer_car_, race_rows_);
        viewer_paint_ = viewer_choice(page, paints, viewer.variant_choice, &AssetViewer::variant_choice);
        viewer_row(page, "Paint:", viewer_paint_, race_rows_);
        viewer_lod_ = viewer_choice(page, lods, viewer.lod_choice, &AssetViewer::lod_choice);
        viewer_row(page, "Detail:", viewer_lod_, race_rows_);
        viewer_chassis_ = viewer_choice(page, chassis, viewer.chassis_choice, &AssetViewer::chassis_choice);
        viewer_row(page, "Chassis:", viewer_chassis_, showroom_rows_);
        viewer_tire_ = viewer_choice(page, tires, viewer.tire_choice, &AssetViewer::tire_choice);
        viewer_row(page, "Tires:", viewer_tire_, showroom_rows_);
        top->Add(viewer_grid_, 0, wxALIGN_TOP);
        top->AddStretchSpacer();

        auto* options = new wxBoxSizer(wxVERTICAL);
        viewer_wireframe_ = new wxCheckBox(page, wxID_ANY, "Wireframe");
        viewer_wireframe_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& e) {
            g_asset_viewer.wireframe = e.IsChecked();
            viewport_->Refresh();
        });
        viewer_lighting_ = new wxCheckBox(page, wxID_ANY, "Lighting");
        viewer_lighting_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& e) {
            g_asset_viewer.preview_lighting = e.IsChecked();
            viewport_->Refresh();
        });
        auto* reset_camera = new wxButton(page, wxID_ANY, "Reset Camera");
        reset_camera->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            g_asset_viewer.yaw = kDefaultYaw;
            g_asset_viewer.pitch = kDefaultPitch;
            g_asset_viewer.zoom = 1.0f;
            viewport_->Refresh();
        });
        options->Add(viewer_wireframe_, 0, wxBOTTOM, FromDIP(4));
        options->Add(viewer_lighting_, 0, wxBOTTOM, FromDIP(6));
        options->Add(reset_camera, 0);
        top->Add(options, 0, wxALIGN_TOP);
        sizer->Add(top, 0, wxEXPAND | wxALL, FromDIP(12));

        viewport_ = new CarViewport(page);
        sizer->Add(viewport_, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
        auto* footer = new wxBoxSizer(wxHORIZONTAL);
        viewer_stats_ = new wxStaticText(page, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                         wxST_ELLIPSIZE_END);
        footer->Add(viewer_stats_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        footer->Add(new wxStaticText(page, wxID_ANY, "Drag to orbit, scroll to zoom."), 0,
                    wxALIGN_CENTER_VERTICAL);
        sizer->Add(footer, 0, wxEXPAND | wxALL, FromDIP(12));
        page->SetSizer(sizer);
        return page;
    }

    // ---- Settings <-> controls ---------------------------------------------

    // wxOSX sizes a pop-up to its widest item's bare text, which Cocoa then
    // truncates once the bezel and arrow are drawn ("Sharp bilinear" ->
    // "Sharp biline..."). Measure every item and add room for that chrome so
    // each option is readable; short lists keep a common minimum width.
    static void fit_choice_width(wxChoice* choice) {
        choice->InvalidateBestSize();
        int widest = 0;
        for (unsigned i = 0; i < choice->GetCount(); ++i)
            widest = std::max(widest, choice->GetTextExtent(choice->GetString(i)).x);
        const int width = std::max({choice->GetBestSize().x, widest + choice->FromDIP(44),
                                    choice->FromDIP(110)});
        choice->SetMinSize(wxSize(width, -1));
    }

    static void fill_flag_choice(const FlagChoice& c, const Settings& s) {
        c.choice->Clear();
        for (int i = 0; i < c.count; ++i) c.choice->Append(wx_text(c.names[i]));
        const int row = find_flag(s, c.flag);
        // A disabled string flag reads as unset; level flags keep their value.
        const char* value = row >= 0 && (c.levels || s.flags[size_t(row)].enabled)
                                ? s.flags[size_t(row)].value.data()
                                : "";
        const int index = c.levels ? level_value_index(value, c.levels, c.count)
                                   : choice_value_index(value, c.strings, c.count);
        if (index < 0 && value[0]) {
            c.choice->Append("Custom (" + wx_text(value) + ")");
            c.choice->SetSelection(c.count);
        } else {
            c.choice->SetSelection(std::max(index, 0));
        }
        fit_choice_width(c.choice);
    }

    static void set_texture_filtering(Settings& s, const TextureFilteringChoice& c) {
        if (const int row = find_flag(s, "M2_TEXTURE_FILTER"); row >= 0) {
            auto& f = s.flags[size_t(row)];
            copy(f.value, c.filter);
            f.enabled = c.filter[0] != '\0';
        }
        if (const int row = find_flag(s, "M2_ANISOTROPY"); row >= 0) {
            auto& f = s.flags[size_t(row)];
            copy(f.value, std::to_string(c.anisotropy));
            f.enabled = c.anisotropy > 1;
        }
    }

    // Shows the combined texture filtering choice. A saved combination with a
    // nearest entry (sharp textures, or anisotropy over Original) is rewritten
    // to that entry so the launch matches the menu; a hand-typed value outside
    // the ladder is listed as custom and left untouched.
    void fill_texture_filtering() {
        auto exported = [this](const char* flag) {
            const int row = find_flag(settings_, flag);
            if (row < 0) return std::string();
            const auto& f = settings_.flags[size_t(row)];
            return should_export_flag(f.enabled, f.value.data()) ? std::string(f.value.data())
                                                                 : std::string();
        };
        const std::string filter = exported("M2_TEXTURE_FILTER");
        const std::string anisotropy = exported("M2_ANISOTROPY");
        texture_filtering_->Clear();
        for (const auto& c : kTextureFilteringChoices) texture_filtering_->Append(wx_text(c.name));
        const int index = texture_filtering_index(filter.c_str(), anisotropy.c_str());
        if (index < 0) {
            std::string custom = filter;
            if (!anisotropy.empty()) custom += (custom.empty() ? "" : ", ") + anisotropy + "x";
            texture_filtering_->Append("Custom (" + wx_text(custom) + ")");
            texture_filtering_->SetSelection(kTextureFilteringChoiceCount);
        } else {
            set_texture_filtering(settings_, kTextureFilteringChoices[index]);
            texture_filtering_->SetSelection(index);
        }
        fit_choice_width(texture_filtering_);
    }

    // Full screen takes over the window size, and Borderless only qualifies
    // full screen, so each is greyed out while it has no effect.
    void update_display_controls() {
        resolution_->Enable(!settings_.fullscreen);
        if (borderless_) borderless_->Enable(settings_.fullscreen);
    }

    // Shows the loaded settings; a saved value outside the presets is listed
    // as custom rather than silently replaced.
    void load_settings_into_controls() {
        resolution_->Clear();
        for (const auto& preset : kResolutionPresets) resolution_->Append(wx_text(preset.label));
        const int res = resolution_index(settings_.width, settings_.height);
        if (res < 0) {
            resolution_->Append(wxString::Format(wx_text("Custom (%d×%d)"), settings_.width, settings_.height));
            resolution_->SetSelection(kResolutionPresetCount);
        } else {
            resolution_->SetSelection(res);
        }
        resolution_->InvalidateBestSize();
        fullscreen_->SetValue(settings_.fullscreen);
        if (borderless_) borderless_->SetValue(settings_.borderless);
        for (int i = 0; i < 2; ++i)
            controller_type_[i]->SetValue((settings_.controller_type == ControllerType::Joystick ? 1 : 0) == i);
        controls_modes_->ChangeSelection(controls_tab(settings_.controls_mode));
        rebuild_controls_rows();
        update_display_controls();
        for (const auto& c : flag_choices_) fill_flag_choice(c, settings_);
        fill_texture_filtering();
        for (const auto& [flag, box] : flag_boxes_) box->SetValue(option_is_on(settings_, flag));
    }

    // ---- Refresh ------------------------------------------------------------

    // Enables, labels and visibility that follow the launcher's state.
    void refresh_state() {
        refresh_screenshot_help();
        const bool busy_or_invalid = process_ != nullptr || !settings_valid_ || !controls_valid_;
        const bool cannot_play = busy_or_invalid || !game_ready_ || importing_.valid();
        play_button_->Enable(!cannot_play);
        shooter_button_->Enable(!cannot_play);
        reset_invalid_button_->Show(!settings_valid_);

        const bool importing = importing_.valid();
        setup_status_->SetLabel(wx_text(game_ready_ ? "Game installed."
                                : importing ? "Installing…"
                                            : "Game not installed."));
        setup_prompt_->Show(!game_ready_ && !importing);
        choose_iso_->Show(!game_ready_ && !importing);
        choose_iso_->Enable(!process_);
        import_gauge_->Show(importing);
        cancel_import_->Show(importing);
        install_label_->Show(game_ready_);
        install_path_->Show(game_ready_);

        if (!save_summary_.error.empty()) {
            save_summary_text_->SetLabel(wx_text(save_summary_.error));
        } else if (!save_summary_.files) {
            save_summary_text_->SetLabel("No save data yet.");
        } else {
            std::ostringstream s;
            s << save_summary_.files << " files, " << std::fixed << std::setprecision(1)
              << (save_summary_.bytes / 1024.0) << " KB of MicroCard storage";
            save_summary_text_->SetLabel(wx_text(s.str()));
        }
        const bool can_modify_saves = process_ == nullptr && save_summary_.error.empty() && save_summary_.files > 0;
        create_backup_->Enable(can_modify_saves);
        reset_saves_->Enable(can_modify_saves);
        restore_backup_->Enable(process_ == nullptr && !save_backups_.empty() && !selected_backup_.empty());

        for (const auto& [id, page] : pages_) page->FitInside();
        GetChildren().front()->Layout();
        show_message();
    }

    void show_message() {
        const wxString text = wx_text(message_);
        if (text != shown_message_) {
            SetStatusText(text);
            shown_message_ = text;
        }
    }

    void rebuild_controls_rows() {
        actions_list_->DeleteAllItems();
        row_actions_.clear();
        const bool custom = settings_.controls_mode == M2_CONTROLS_CUSTOM;
        for (const auto& section : kActionRowSections) {
            for (size_t r = 0; r < section.count; ++r) {
                const auto action = section.rows[r];
                if (action != Action::Quit && (action >= Action::DriveLeft) != custom) continue;
                if (action >= Action::MenuUp && action <= Action::MenuRight) continue;
                row_actions_.push_back(action);
                wxVector<wxVariant> row;
                row.push_back(wxVariant(wx_text(action_row_label(action))));
                row.push_back(wxVariant(wxString())); row.push_back(wxVariant(wxString()));
                row.push_back(wxVariant(wxString()));
                row.push_back(wxVariant(long(0)));
                actions_list_->AppendItem(row);
            }
        }
        shown_levels_.assign(row_actions_.size(), 0);
        actions_list_->SelectRow(0);
        refresh_controls_list();
    }

    void refresh_controls_list() {
        actions_list_->UnsetToolTip();
        if (controls_visibility_mode_ != settings_.controls_mode) {
            controls_visibility_mode_ = settings_.controls_mode;
            const bool custom = settings_.controls_mode == M2_CONTROLS_CUSTOM;
            actions_list_->GetColumn(2)->SetHidden(custom);
            actions_list_->GetColumn(3)->SetHidden(custom);
            actions_list_->GetColumn(1)->SetWidth(FromDIP(custom ? 240 : 190));
            auto* page = actions_list_->GetParent();
            page->GetSizer()->Show(controls_type_row_, !custom, true);
            // Showing the row recursively also shows both groups; restore their visibility.
            controls_type_row_->Show(controller_type_group_, settings_.controls_mode == M2_CONTROLS_ORIGINAL, true);
            controls_type_row_->Show(patched_preview_group_, settings_.controls_mode == M2_CONTROLS_PATCHED, true);
            page->FitInside();
        }
        for (auto* btn : controller_type_)
            btn->Enable(settings_.controls_mode == M2_CONTROLS_ORIGINAL);
        for (auto* btn : patched_preview_)
            btn->Enable(settings_.controls_mode == M2_CONTROLS_PATCHED);
        for (size_t row = 0; row < row_actions_.size(); ++row) {
            std::string bound;
            for (const auto& control : controls_.bindings) {
                if (control.action != row_actions_[row]) continue;
                if (!bound.empty()) bound += ", ";
                bound += controls_.describe(control.binding);
            }
            actions_list_->SetTextValue(bound.empty() ? wxString("Not bound") : wx_text(bound), row, 1);
            actions_list_->SetTextValue(wx_text(action_effect(row_actions_[row], settings_.controls_mode,
                settings_.controller_type, patched_preview_selection_)), row, 2);
            actions_list_->SetTextValue(wx_text(shooter_action_effect(row_actions_[row])), row, 3);
        }
        std::string devices = "Devices: Keyboard and Mouse";
        for (const auto& d : controls_.devices()) devices += ", " + d;
        devices_->SetLabel(wx_text(devices));
        refresh_binding_controls();
    }

    int selected_action() const {
        const int row = actions_list_->GetSelectedRow();
        return row >= 0 && size_t(row) < row_actions_.size() ? int(row_actions_[size_t(row)]) : -1;
    }

    bool screenshot_row(Action action) const {
        return is_screenshot_action(action, settings_.controls_mode, settings_.controller_type,
                                    patched_preview_selection_);
    }

    wxString control_tooltip(Action action) const {
        if (screenshot_row(action)) return screenshot_help_text_;
        if (is_hud_cycle_action(action, settings_.controls_mode, settings_.controller_type,
                                patched_preview_selection_)) return wx_text(hud_cycle_tooltip());
        return {};
    }

    // Recompute the screenshot destination after the selected host or
    // M2_SCREENSHOT_ROOT changes, and refresh the visible help when the
    // screenshot row is the one currently selected.
    void refresh_screenshot_help() {
        screenshot_help_text_ = wx_text(screenshot_tooltip(screenshot_directory(settings_)));
        refresh_binding_controls();
    }

    void refresh_binding_controls() {
        const int action = selected_action();
        const wxString tip = action >= 0 ? control_tooltip(Action(action)) : wxString();
        action_help_->Show(!tip.empty());
        if (!tip.empty()) {
            action_help_->SetLabel(wx_text(screenshot_row(Action(action)) ? "Screenshot info ⓘ" : "HUD displays info ⓘ"));
            action_help_->SetToolTip(tip);
        }
        selected_box_->Layout();
        selected_box_->GetStaticBox()->SetLabel(
            action < 0 ? wxString("Selected action")
                       : "Selected action: " + wx_text(action_row_label(Action(action))));
        bool has_binding = false;
        const HostBinding* axis = nullptr;
        for (const auto& control : controls_.bindings) {
            if (int(control.action) != action) continue;
            has_binding = true;
            if (control.binding.kind == HostBinding::JoyAxis && !axis) axis = &control.binding;
        }
        bind_button_->Enable(action >= 0);
        add_binding_->Enable(has_binding);
        clear_binding_->Enable(has_binding);
        deadzone_->Enable(axis != nullptr);
        if (axis) deadzone_->SetValue(axis->deadzone);
    }

    void refresh_saves() {
        save_summary_ = launcher_saves::inspect(saves_root_);
        try {
            save_backups_ = launcher_saves::backups(backups_root_);
        } catch (const std::exception& e) {
            message_ = e.what();
        }
        if (std::find(save_backups_.begin(), save_backups_.end(), selected_backup_) == save_backups_.end())
            selected_backup_ = save_backups_.empty() ? fs::path{} : save_backups_.front();
        backups_list_->Clear();
        for (const auto& path : save_backups_) {
            auto text = path.filename().string().substr(0, 19);
            if (text.size() == 19) { text[10] = ' '; text[13] = ':'; text[16] = ':'; }
            backups_list_->Append(wx_text(text + "  (" + path.filename().string() + ")"));
            if (path == selected_backup_) backups_list_->SetSelection(backups_list_->GetCount() - 1);
        }
        refresh_state();
    }

    void refresh_viewer() {
        AssetViewer& viewer = g_asset_viewer;
        viewer_sync_models(viewer, path_to_utf8(viewer_data_root()));
        for (wxWindow* w : race_rows_) w->Show(viewer.race_models);
        for (wxWindow* w : showroom_rows_) w->Show(!viewer.race_models);
        viewer_mode_->SetSelection(viewer.race_models ? 1 : 0);
        viewer_wireframe_->SetValue(viewer.wireframe);
        viewer_lighting_->SetValue(viewer.preview_lighting);

        viewer.part_choice = -1;
        if (viewer.chassis.race_pack) {
            for (size_t i = 0; i < viewer.chassis.parts.size(); ++i) {
                const auto& part = viewer.chassis.parts[i];
                if (part.component == "body" && part.body == viewer.body_choice &&
                    part.variant == viewer.variant_choice && part.lod == viewer.lod_choice)
                    viewer.part_choice = int(i);
            }
        }
        const auto* shown_part = viewer_part(viewer.chassis, viewer.part_choice);
        if (shown_part) {
            const auto& mesh = shown_part->mesh;
            size_t textured = 0;
            for (const uint32_t page : shown_part->triangle_texture)
                if (page != launcher_assets::kNoTexture) ++textured;
            std::ostringstream stats;
            stats << viewer.message << ": " << mesh.positions.size() << " vertices, "
                  << (mesh.indices.size() / 3) << " triangles (" << textured << " textured)";
            viewer_stats_->SetLabel(wx_text(stats.str()));
        } else {
            viewer_stats_->SetLabel(wx_text(viewer.message));
        }
        viewport_->GetParent()->FitInside();
        viewport_->Refresh();
    }

    // ---- Actions ------------------------------------------------------------

    bool auto_save_settings() {
        if (!settings_valid_) return false;
        std::string err;
        if (!save_settings(settings_path_, settings_, err)) {
            message_ = err;
            show_message();
            return false;
        }
        return true;
    }

    bool auto_save_controls() {
        if (!controls_valid_) return false;
        std::string err;
        if (!controls_.save(profile_, err)) {
            message_ = err;
            show_message();
            return false;
        }
        return true;
    }

    void reset_invalid_settings() {
        settings_ = Settings{};
        ensure_launcher_flags(settings_);
        apply_install_paths();
        settings_valid_ = true;
        auto_save_settings();
        message_ = "Launcher settings reset.";
        load_settings_into_controls();
        refresh_state();
    }

    void launch_game(bool shooter2d) {
        if (auto_save_controls() && auto_save_settings()) {
            process_ = launch(settings_, profile_, message_, shooter2d, saves_root_);
        }
        refresh_state();
    }

    void choose_iso() {
        if (importing_.valid() || process_) return;
        wxFileDialog dialog(this, "Choose your IMSA Racing ISO", "", "",
                            "Disc images (*.iso)|*.iso;*.ISO|All files|*",
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) return;
        const fs::path iso =
            m2host::platform::path_from_utf8(dialog.GetPath().ToStdString(wxConvUTF8));
        import_progress_.fraction = 0;
        import_progress_.cancel = false;
        message_ = "Installing…";
        importing_ = std::async(std::launch::async, [iso, root = game_root_, progress = &import_progress_] {
            return launcher_disc::import_iso(iso, root, *progress);
        });
        import_gauge_->SetValue(0);
        refresh_state();
    }

    void open_folder(const fs::path& path) {
        std::error_code ec;
        fs::create_directories(path, ec);
        if (ec) {
            message_ = ec.message();
        } else if (!wxLaunchDefaultApplication(
                   wxString::FromUTF8(path_to_utf8(fs::absolute(path, ec))))) {
            message_ = "Could not open " + path_to_utf8(path);
        }
        show_message();
    }

    bool confirm(const wxString& title, const wxString& body, const wxString& proceed) {
        wxMessageDialog dialog(this, body, title, wxOK | wxCANCEL | wxCANCEL_DEFAULT | wxICON_WARNING);
        dialog.SetOKCancelLabels(proceed, "Cancel");
        return dialog.ShowModal() == wxID_OK;
    }

    void confirm_restore() {
        if (selected_backup_.empty()) return;
        if (!confirm("Restore the selected backup?", "Your current saves are backed up first.",
                     "Back Up and Restore"))
            return;
        message_ = launcher_saves::restore(saves_root_, backups_root_, selected_backup_).message;
        refresh_saves();
    }

    void confirm_reset() {
        if (!confirm("Reset all save data?", "Your current saves are backed up first.",
                     "Back Up and Reset"))
            return;
        message_ = launcher_saves::reset(saves_root_, backups_root_).message;
        refresh_saves();
    }

    void clear_selected_binding() {
        const int action = selected_action();
        if (action < 0) return;
        controls_.clear_bindings(Action(action));
        auto_save_controls();
        message_ = std::string("Cleared binding for ") + action_row_label(Action(action)) + ".";
        refresh_controls_list();
        show_message();
    }

    void set_deadzone(float value) {
        const int action = selected_action();
        if (action < 0) return;
        const float deadzone = std::clamp(std::round(value * 100.0f) / 100.0f, 0.0f, 0.5f);
        bool changed = false;
        for (auto& control : controls_.bindings) {
            if (int(control.action) != action || control.binding.kind != HostBinding::JoyAxis) continue;
            control.binding.deadzone = deadzone;
            changed = true;
        }
        if (changed) auto_save_controls();
    }

    // ---- Binding capture ----------------------------------------------------

    void start_capture(bool add = false) {
        const int action_number = selected_action();
        if (action_number < 0 || capture_dialog_) return;
        const Action action = Action(action_number);
        if (controls_.bindings.size() >= HostControls::kMaxBindings &&
            (add || std::none_of(controls_.bindings.begin(), controls_.bindings.end(),
                                [action](const HostControl& c) { return c.action == action; }))) {
            message_ = "Clear an existing binding before adding another.";
            show_message();
            return;
        }
        adding_binding_ = add;
        const wxString action_label = wx_text(action_row_label(action));
        CaptureDialog dialog(this, action_label);
        capture_dialog_ = &dialog;
        captured_.reset();
        // Ignore the click or key that opened the prompt.
        capture_after_ = SDL_GetTicks() + 250;
        controls_.start_capture();
        const int result = dialog.ShowModal();
        capture_dialog_ = nullptr;
        controls_.reset();
        if (result == wxID_OK && captured_) {
            // Moving an input that another action already uses is explicit:
            // the captured input replaces this action's current binding, and
            // the other action loses it only after confirmation.
            std::vector<std::string> conflicts;
            for (const auto& control : controls_.bindings) {
                if (control.action == action || !same_input(control.binding, *captured_)) continue;
                if (!binding_context_overlaps(action, control.action)) continue;
                const std::string label = action_row_label(control.action);
                if (std::find(conflicts.begin(), conflicts.end(), label) == conflicts.end())
                    conflicts.push_back(label);
            }
            bool apply = true;
            bool move = true;
            if (!conflicts.empty()) {
                std::string joined;
                for (const auto& conflict : conflicts)
                    joined += (joined.empty() ? "" : ", ") + conflict;
                if (settings_.controls_mode == M2_CONTROLS_CUSTOM) {
                    wxMessageDialog choice(this, "This input is already bound to " + wx_text(joined) +
                        ". Share it with " + action_label + ", or move it?", "Shared input",
                        wxYES_NO | wxCANCEL | wxICON_QUESTION);
                    choice.SetYesNoCancelLabels("Share input", "Move input", "Cancel");
                    const int answer = choice.ShowModal();
                    apply = answer != wxID_CANCEL;
                    move = answer == wxID_NO;
                } else {
                    apply = confirm("Reassign input?",
                                    "This input is already bound to " + wx_text(joined) + ". Move it to " +
                                        action_label + "?", "Reassign");
                }
            }
            if (apply) {
                if (move && !conflicts.empty())
                    controls_.bindings.erase(
                        std::remove_if(controls_.bindings.begin(), controls_.bindings.end(),
                                       [&](const HostControl& control) {
                                           return control.action != action &&
                                                  binding_context_overlaps(action, control.action) &&
                                                  same_input(control.binding, *captured_);
                                       }),
                        controls_.bindings.end());
                if (adding_binding_) {
                    const bool duplicate = std::any_of(controls_.bindings.begin(), controls_.bindings.end(),
                        [&](const HostControl& control) { return control.action == action && same_input(control.binding, *captured_); });
                    if (!duplicate)
                        controls_.bindings.push_back({action, *captured_});
                    controls_.reset();
                } else controls_.set_binding(action, *captured_);
                auto_save_controls();
                message_ = "Bound " + std::string(action_row_label(action)) + " to " +
                           controls_.describe(*captured_) + ".";
            } else {
                message_ = "Binding unchanged.";
            }
        } else if (result == wxID_OK) {
            message_ = "No input captured.";
        } else {
            message_ = "Binding capture canceled.";
        }
        captured_.reset();
        refresh_controls_list();
        show_message();
    }

    bool capture_ready() const { return capture_dialog_ && SDL_GetTicks() >= capture_after_; }

    // Offers an input to an open capture prompt; true when it became the binding.
    bool offer_capture(const SDL_Event& e) {
        HostBinding b;
        if (!capture_ready() || captured_ || !controls_.capture(e, b)) return false;
        captured_ = b;
        capture_dialog_->EndModal(wxID_OK);
        return true;
    }

    bool key_event(wxKeyEvent& key) {
        const SDL_Scancode scancode = launcher_keys::scancode(key);
        if (scancode == SDL_SCANCODE_UNKNOWN) return false;
        const int modifier = launcher_keys::modifier_down(key, scancode);
        const bool down = modifier >= 0 ? modifier == 1 : key.GetEventType() != wxEVT_KEY_UP;
        const bool repeat = down && held_keys_.count(scancode);
        if (down) held_keys_.insert(scancode);
        else held_keys_.erase(scancode);
        SDL_Event e{};
        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.scancode = scancode;
        e.key.down = down;
        e.key.repeat = repeat;
        if (!repeat) controls_.event(e);
        if (!capture_dialog_ || !down) return false;
        // Escape belongs to the dialog: it cancels the prompt.
        if (scancode == SDL_SCANCODE_ESCAPE) return false;
        if (!repeat) offer_capture(e);
        return true;  // keys never reach the dialog's buttons while capturing
    }

    bool mouse_event(wxMouseEvent& mouse) {
        const wxEventType type = mouse.GetEventType();
        auto* window = dynamic_cast<wxWindow*>(mouse.GetEventObject());
        SDL_Event e{};
        if (mouse.ButtonDown() || mouse.ButtonUp()) {
            const int button = mouse.GetButton();
            if (button < 1 || button > 5) return false;
            const bool down = mouse.ButtonDown();
            e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            e.button.button = Uint8(button);  // wx and SDL share 1..5 = L, M, R, X1, X2
            e.button.down = down;
        } else if (type == wxEVT_MOUSEWHEEL) {
            const float steps = mouse.GetWheelDelta() ? float(mouse.GetWheelRotation()) / mouse.GetWheelDelta() : 0.0f;
            e.type = SDL_EVENT_MOUSE_WHEEL;
            (mouse.GetWheelAxis() == wxMOUSE_WHEEL_HORIZONTAL ? e.wheel.x : e.wheel.y) = steps;
            e.wheel.direction = mouse.IsWheelInverted() ? SDL_MOUSEWHEEL_FLIPPED : SDL_MOUSEWHEEL_NORMAL;
        } else if (type == wxEVT_MOTION && window) {
            const wxPoint screen = window->ClientToScreen(mouse.GetPosition());
            if (last_mouse_) {
                e.type = SDL_EVENT_MOUSE_MOTION;
                e.motion.xrel = float(screen.x - last_mouse_->x);
                e.motion.yrel = float(screen.y - last_mouse_->y);
            }
            last_mouse_ = screen;
            if (!e.type) return false;
        } else {
            return false;
        }
        controls_.event(e);
        // Clicks and scrolls inside the prompt bind, except on its own buttons.
        if (!capture_dialog_ || e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_UP ||
            !window || wxGetTopLevelParent(window) != capture_dialog_ || dynamic_cast<wxButton*>(window))
            return false;
        offer_capture(e);
        return true;
    }

    void release_inputs() {
        held_keys_.clear();
        last_mouse_.reset();
        controls_.reset();
    }

    // ---- Periodic work --------------------------------------------------------

    void tick() {
        bool devices_changed = false;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            controls_.event(e);
            if (e.type == SDL_EVENT_JOYSTICK_ADDED || e.type == SDL_EVENT_JOYSTICK_REMOVED)
                devices_changed = true;
            offer_capture(e);
        }
        if (devices_changed) refresh_controls_list();

        if (importing_.valid()) {
            const int pct = std::clamp(int(std::round(import_progress_.fraction.load() * 100.0f)), 0, 100);
            if (import_gauge_->GetValue() != pct) import_gauge_->SetValue(pct);
            if (importing_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                const auto result = importing_.get();
                game_ready_ = result.ok;
                message_ = result.message;
                refresh_state();
                refresh_viewer();
            }
        }

        if (process_) {
            int exit_code;
            if (SDL_WaitProcess(process_, false, &exit_code)) {
                SDL_DestroyProcess(process_);
                process_ = nullptr;
                message_ = "Game exited with code " + std::to_string(exit_code) + ".";
                controls_.reset();
                refresh_saves();
            }
        }

        if (current_tab() == "controls") {
            const auto levels = controls_.levels();
            for (size_t row = 0; row < row_actions_.size(); ++row) {
                const int pct = std::clamp(int(std::round(levels[size_t(row_actions_[row])] * 100.0f)), 0, 100);
                if (pct != shown_levels_[row]) {
                    actions_list_->SetValue(wxVariant(long(pct)), unsigned(row), 4);
                    shown_levels_[row] = pct;
                }
            }
        }
        show_message();
        controls_.end_frame();
    }

    void on_close(wxCloseEvent& event) {
        timer_.Stop();
        if (importing_.valid()) {
            import_progress_.cancel = true;
            importing_.get();
        }
        auto_save_controls();
        auto_save_settings();
        // The game keeps running when the launcher closes.
        if (process_) SDL_DestroyProcess(process_);
        process_ = nullptr;
        g_frame = nullptr;
        event.Skip();
    }
};

class LauncherApp final : public wxApp {
public:
    fs::path settings_dir;
    std::string initial_tab = "game";

    bool OnInit() override {
        SetAppDisplayName("IMSA WCR M2 Recomp Launcher");
        g_frame = new LauncherFrame(settings_dir, initial_tab);
        apply_launcher_icon(*g_frame);
        g_frame->Show();
        return true;
    }

    int FilterEvent(wxEvent& event) override {
        return g_frame && g_frame->filter_input(event) ? Event_Processed : Event_Skip;
    }
};

} // namespace

wxIMPLEMENT_APP_NO_MAIN(LauncherApp);

int main(int argc, char** argv) {
    fs::path settings_dir;
    fs::path preview_input, preview_output, preview_tire;
    std::string initial_tab;
    int preview_part = -1;
    float preview_yaw = kDefaultYaw, preview_pitch = kDefaultPitch;
    bool select_asset_tab = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--settings-dir" && i + 1 < argc) settings_dir = argv[++i];
        else if (arg == "--car-viewer") {
            select_asset_tab = true;
            g_asset_viewer.race_models = true;
            g_asset_viewer.chassis_choice = 0;
        }
        else if (arg == "--tab" && i + 1 < argc) initial_tab = argv[++i];
        else if (arg == "--preview-asset" && i + 1 < argc) preview_input = argv[++i];
        else if (arg == "--preview-output" && i + 1 < argc) preview_output = argv[++i];
        else if (arg == "--preview-part" && i + 1 < argc) preview_part = std::atoi(argv[++i]);
        else if (arg == "--preview-yaw" && i + 1 < argc) preview_yaw = std::strtof(argv[++i], nullptr);
        else if (arg == "--preview-pitch" && i + 1 < argc) preview_pitch = std::strtof(argv[++i], nullptr);
        else if (arg == "--preview-tire" && i + 1 < argc) preview_tire = argv[++i];
        else {
            SDL_Log("Usage: m2launcher [--settings-dir DIRECTORY] [--car-viewer] [--tab game|graphics|controls|enhancements|saves|viewer]\n"
                    "       m2launcher --preview-asset MEMIMAGE --preview-output FILE.bmp [--preview-part N] [--preview-yaw RADIANS] [--preview-pitch RADIANS] [--preview-tire MEMIMAGE]");
            return arg == "--help" ? 0 : 2;
        }
    }

    if (!preview_input.empty()) {
        if (preview_output.empty()) preview_output = "asset-preview.bmp";
        std::error_code preview_ec;
        if (!preview_output.parent_path().empty())
            fs::create_directories(preview_output.parent_path(), preview_ec);
        if (preview_ec) {
            SDL_Log("%s", preview_ec.message().c_str());
            return 1;
        }
        return preview_asset(preview_input, preview_output, preview_part,
                             preview_yaw, preview_pitch, preview_tire);
    }

    // No SDL window ever exists here, so joystick events must not depend on
    // SDL's notion of focus.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_JOYSTICK)) {
        SDL_Log("%s", SDL_GetError());
        return 1;
    }

    if (settings_dir.empty()) settings_dir = launcher_settings_directory();
    if (settings_dir.empty()) {
        SDL_Log("Cannot locate user settings directory");
        SDL_Quit();
        return 1;
    }
    std::error_code ec;
    fs::create_directories(settings_dir, ec);
    if (ec || !m2host::platform::directory_writable(settings_dir)) {
        // The launcher has no window yet, and a log line is invisible to
        // someone who double-clicked it.
        const std::string error = "Cannot write to this folder:\n\n" + path_to_utf8(settings_dir) +
            "\n\nMove the game somewhere you can write to and try again.";
        SDL_Log("%s", error.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "IMSA WCR M2 Recomp", error.c_str(), nullptr);
        SDL_Quit();
        return 1;
    }
    settings_dir = fs::absolute(settings_dir);
    const bool game_available = launcher_disc::installed(launcher_disc::install_root(settings_dir));
    if (initial_tab.empty()) initial_tab = select_asset_tab && game_available ? "viewer" : "game";

    wxApp::SetInstance(new LauncherApp);
    auto* app = static_cast<LauncherApp*>(wxApp::GetInstance());
    app->settings_dir = settings_dir;
    app->initial_tab = initial_tab;
    // wxWidgets parses argv itself; the launcher's own options are consumed above.
    int wx_argc = 1;
    const int result = wxEntry(wx_argc, argv);
    SDL_Quit();
    return result;
}
