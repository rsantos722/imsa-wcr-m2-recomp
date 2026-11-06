// Host-only launcher settings model: storage, persistence, launch arguments,
// the curated run-flag options behind the Graphics and Enhancements tabs,
// and the Controls-tab row model. ImGui-free so the option, merge, and
// load/save logic can be exercised without the GUI.
#pragma once

#include <SDL3/SDL.h>

#include "input_map.h"
#include "control_modes.h"
#include "flags_catalog.h"
#include "platform.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <string>
#include <vector>

namespace launcher_flags {
struct RunFlag { bool enabled = false; std::array<char, 128> name{}; std::array<char, 512> value{}; };
// Guest-facing controller device, independent of the physical bindings. The
// game's own mode byte selects Cpad (digital) or Joy (analog) from this.
enum class ControllerType { Pad = 0, Joystick = 1 };
inline const char* controller_type_value(ControllerType type) {
    return type == ControllerType::Joystick ? "joystick" : "pad";
}
inline const char* controller_type_label(ControllerType type) {
    return type == ControllerType::Joystick ? "Analog" : "Digital";
}
struct Settings {
    std::array<char, 2048> executable{}, data{};
    bool fast_forward = false;
    bool use_host_defaults = false;
    int width = 960, height = 720;
    // Fullscreen replaces the window size; borderless picks a desktop-sized
    // borderless window over exclusive fullscreen. The host always keeps the
    // game's 4:3 aspect with black bars.
    bool fullscreen = false;
    bool borderless = false;
    ControllerType controller_type = ControllerType::Pad;
    m2_controls_mode controls_mode = M2_CONTROLS_CUSTOM; // Custom is the launcher default.
    std::vector<RunFlag> flags;
};
template<size_t N> inline void copy(std::array<char, N>& to, const std::string& s) { SDL_strlcpy(to.data(), s.c_str(), N); }

// The managed data root is driving/data; the second disc application keeps
// its relative assets in the sibling shooter2d directory.
inline std::filesystem::path launch_data_root(const Settings& s, bool shooter2d) {
    auto data = std::filesystem::absolute(m2host::platform::path_from_utf8(s.data.data())).lexically_normal();
    if (data.filename().empty()) data = data.parent_path();
    return shooter2d ? data.parent_path().parent_path() / "shooter2d" : data;
}

inline std::vector<std::string> launch_arguments(const Settings& s,
                                                  const std::filesystem::path& profile,
                                                  bool shooter2d = false) {
    // SDL_CreateProcess takes UTF-8 arguments; path::string() is the ANSI code
    // page on Windows. m2host reads them back with platform::utf8_args.
    using m2host::platform::path_to_utf8;
    std::vector<std::string> argv = {
        path_to_utf8(std::filesystem::absolute(m2host::platform::path_from_utf8(s.executable.data())))};
    argv.insert(argv.end(), {"--root", path_to_utf8(launch_data_root(s, shooter2d))});
    // Custom has shooter2d bindings. Patched only changes racing's controls.
    const bool pass_mode = s.controls_mode == M2_CONTROLS_CUSTOM ||
                           (!shooter2d && s.controls_mode == M2_CONTROLS_PATCHED);
    if (pass_mode)
        argv.insert(argv.end(), {"--controls-mode", s.controls_mode == M2_CONTROLS_PATCHED ? "patched" : "custom"});
    const auto type = pass_mode ? ControllerType::Pad : s.controller_type;
    if (s.use_host_defaults) {
        argv.insert(argv.end(), {"--controller-type", controller_type_value(type)});
        return argv;
    }
    argv.insert(argv.end(), {"--width", std::to_string(s.width), "--height", std::to_string(s.height),
                             "--controls-profile", path_to_utf8(profile)});
    if (s.fullscreen) argv.push_back(s.borderless ? "--borderless-fullscreen" : "--fullscreen");
    if (s.fast_forward) argv.push_back("--fast-forward");
    argv.insert(argv.end(), {"--controller-type", controller_type_value(type)});
    return argv;
}

// The application choice applies even when other host options use defaults.
// Copying the environment for the child keeps the launcher process unchanged.
inline bool apply_save_option(const std::filesystem::path& root, SDL_Environment* environment) {
    // SDL environments are UTF-8; path::string() is the ANSI code page on Windows.
    return !root.empty() && SDL_SetEnvironmentVariable(environment, "M2_MICROCARD_ROOT",
                                                       m2host::platform::path_to_utf8(root).c_str(), true);
}

inline bool apply_boot_option(bool shooter2d, SDL_Environment* environment) {
    return SDL_SetEnvironmentVariable(environment, "M2_BOOT_APP",
                                      shooter2d ? "shooter2d" : "game", true);
}

inline bool save_settings(const std::filesystem::path& path, const Settings& s, std::string& message) {
    using m2host::platform::path_to_utf8;
    std::filesystem::path pending = path;
    pending += ".new";
    std::ofstream f(pending);
    f << "m2-launcher 1\n" << std::quoted(s.executable.data()) << '\n' << std::quoted(s.data.data()) << '\n'
      << s.width << ' ' << s.height << ' ' << s.fast_forward << '\n'
      << s.use_host_defaults << '\n'
      << s.fullscreen << ' ' << s.borderless << '\n'
      << int(s.controller_type) << '\n' << int(s.controls_mode) << '\n';
    for (const auto& flag : s.flags)
        f << flag.enabled << ' ' << std::quoted(flag.name.data()) << ' ' << std::quoted(flag.value.data()) << '\n';
    f.close();
    // SDL path APIs are UTF-8.
    if (!f || !SDL_RenamePath(path_to_utf8(pending).c_str(), path_to_utf8(path).c_str())) {
        message = "Could not save launcher settings: " + path_to_utf8(path); SDL_RemovePath(path_to_utf8(pending).c_str()); return false;
    }
    return true;
}
inline bool load_settings(const std::filesystem::path& path, Settings& s, std::string& message) {
    std::ifstream f(path); std::string magic, exe, data; int version;
    Settings candidate;
    int use_host_defaults = -1, fullscreen = -1, borderless = -1, controller_type = -1, mode = -1;
    if (!(f >> magic >> version) || magic != "m2-launcher" || version != 1 ||
        !(f >> std::quoted(exe) >> std::quoted(data) >> candidate.width >> candidate.height >> candidate.fast_forward) ||
        candidate.width < 320 || candidate.width > 7680 ||
        candidate.height < 240 || candidate.height > 4320 || exe.size() >= s.executable.size() ||
        data.size() >= s.data.size() ||
        !(f >> use_host_defaults >> fullscreen >> borderless >> controller_type >> mode) ||
        (use_host_defaults != 0 && use_host_defaults != 1) ||
        (fullscreen != 0 && fullscreen != 1) || (borderless != 0 && borderless != 1) ||
        (controller_type != 0 && controller_type != 1) || mode < 0 || mode > 2) {
        message = "Cannot read launcher settings: " + m2host::platform::path_to_utf8(path); return false;
    }
    copy(candidate.executable, exe); copy(candidate.data, data);
    candidate.use_host_defaults = use_host_defaults != 0;
    candidate.fullscreen = fullscreen != 0;
    candidate.borderless = borderless != 0;
    candidate.controller_type = controller_type == 1 ? ControllerType::Joystick : ControllerType::Pad;
    candidate.controls_mode = static_cast<m2_controls_mode>(mode);
    bool enabled; std::string name, value;
    while (f >> enabled) {
        if (!(f >> std::quoted(name) >> std::quoted(value)) || name.size() >= 128 || value.size() >= 512 || candidate.flags.size() >= 512) {
            message = "Invalid run flag in launcher settings"; return false;
        }
        RunFlag flag; flag.enabled = enabled; copy(flag.name, name); copy(flag.value, value); candidate.flags.push_back(flag);
    }
    if (!f.eof()) { message = "Invalid launcher settings"; return false; }
    s = std::move(candidate); return true;
}
inline bool valid_flag(const char* name) {
    std::string s(name);
    return s.rfind("M2_", 0) == 0 && s.size() > 3 && s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == std::string::npos;
}
// An enabled row with an empty value must behave as unset: exporting an empty
// environment string reads as "off" to most readers, including the default-on
// platform bridges where checking the box with no value would silently disable
// the feature. The launcher never exports empty values.
inline bool should_export_flag(bool enabled, const char* value) {
    return enabled && value && value[0] != '\0';
}
inline int find_flag(const Settings& s, const char* name) {
    const std::string want(name);
    for (size_t i = 0; i < s.flags.size(); ++i)
        if (s.flags[i].name.data() == want) return int(i);
    return -1;
}
inline bool is_catalog_flag(const char* name) {
    const std::string want(name);
    for (int i = 0; i < kM2LauncherFlagCount; ++i)
        if (kM2LauncherFlags[i].name == want) return true;
    return false;
}
// Launcher Graphics-tab view of an integer-valued presentation catalog row.
// Index 0 is "off" (unset or 1x); the remaining entries are the values the GL
// backends understand. `level_value_index` returns -1 for a hand-typed value
// outside the set, so the UI can show the raw custom value instead of silently
// replacing it.
inline constexpr int kAnisotropyLevelValues[] = {1, 2, 4, 8, 16};
inline constexpr int kAnisotropyLevelCount = 5;
inline constexpr int kAntialiasingLevelValues[] = {1, 2, 4, 8};
inline constexpr const char* kAntialiasingLevelNames[] = {"Off", "2x", "4x",
                                                          "8x"};
inline constexpr int kAntialiasingLevelCount = 4;
// M2_RENDER_SCALE: index 0 follows the window; the others fix the internal
// resolution at N x the guest's 640x480 display.
inline constexpr int kRenderScaleValues[] = {0, 1, 2, 3, 4, 6, 8};
inline constexpr const char* kRenderScaleNames[] = {
    "Match window", "1x (640×480)", "2x (1280×960)", "3x (1920×1440)",
    "4x (2560×1920)", "6x (3840×2880)", "8x (5120×3840)"};
inline constexpr int kRenderScaleCount = 7;
// String-valued presentation filters. Index 0 is the unset original sampling.
inline constexpr const char* kTextureFilterValues[] = {"", "linear", "sharp"};
inline constexpr int kTextureFilterCount = 3;
inline constexpr const char* kBitmapFilterValues[] = {"", "linear", "sharp"};
inline constexpr const char* kBitmapFilterNames[] = {
    "Nearest", "Smooth bilinear", "Sharp bilinear"};
inline constexpr int kBitmapFilterCount = 3;
// Graphics-tab "Texture filtering": one quality ladder over M2_TEXTURE_FILTER
// and M2_ANISOTROPY. Anisotropy above 1 already implies linear sampling in the
// GL backends, so each entry is a combination the host actually renders.
struct TextureFilteringChoice {
    const char* name;
    const char* filter;  // M2_TEXTURE_FILTER value; "" leaves it unset
    int anisotropy;      // M2_ANISOTROPY value; 1 leaves it unset
};
inline constexpr TextureFilteringChoice kTextureFilteringChoices[] = {
    {"Original", "", 1},
    {"Bilinear", "linear", 1},
    {"Anisotropic 2x", "linear", 2},
    {"Anisotropic 4x", "linear", 4},
    {"Anisotropic 8x", "linear", 8},
    {"Anisotropic 16x", "linear", 16},
};
inline constexpr int kTextureFilteringChoiceCount = 6;
// Enhancements view of the player-car body variant. Index 0 is the
// original first variant; the remaining entries are the authored appearance
// variants the opponent wheel also selects from.
inline constexpr int kCarVariantValues[] = {0, 1, 2};
inline constexpr const char* kCarVariantNames[] = {"Original", "Alternate 1",
                                                   "Alternate 2"};
inline constexpr int kCarVariantCount = 3;
inline int level_value_index(const char* value, const int* values, int count) {
    if (!value || !value[0]) return 0;
    for (int i = 0; i < count; ++i)
        if (std::string(value) == std::to_string(values[i])) return i;
    return -1;
}
inline int anisotropy_level_index(const char* value) {
    return level_value_index(value, kAnisotropyLevelValues,
                             kAnisotropyLevelCount);
}
inline int antialiasing_level_index(const char* value) {
    return level_value_index(value, kAntialiasingLevelValues,
                             kAntialiasingLevelCount);
}
inline int render_scale_level_index(const char* value) {
    return level_value_index(value, kRenderScaleValues, kRenderScaleCount);
}
// String counterpart of level_value_index: unset is index 0, an unknown
// hand-typed value is -1 (shown as custom).
inline int choice_value_index(const char* value, const char* const* values,
                              int count) {
    if (!value || !value[0]) return 0;
    for (int i = 0; i < count; ++i)
        if (std::string(value) == values[i]) return i;
    return -1;
}
// Maps the exported M2_TEXTURE_FILTER / M2_ANISOTROPY values ("" when not
// exported) onto kTextureFilteringChoices. Sharp bilinear is no longer offered
// for textures and reads as its nearest entry (Bilinear, or the anisotropic
// level when one is set). Unknown hand-typed values return -1 (custom).
inline int texture_filtering_index(const char* filter, const char* anisotropy) {
    const int level = anisotropy_level_index(anisotropy);
    const int kind = choice_value_index(filter, kTextureFilterValues,
                                        kTextureFilterCount);
    if (level < 0 || kind < 0) return -1;
    if (level > 0) return 1 + level;  // Anisotropic 2x..16x
    return kind == 0 ? 0 : 1;         // Original, or Bilinear for linear/sharp
}
inline int car_variant_level_index(const char* value) {
    return level_value_index(value, kCarVariantValues, kCarVariantCount);
}

// --- Controls-tab rows --------------------------------------------------------
// One labelled row per Action (input_map.h) so a binding can be shown, added,
// or removed in the launcher. Nothing else exposes an action, so the sections
// must partition the whole enum; `action_row_sections_complete()` is asserted
// at compile time here and re-checked by `m2_launcher_settings`.
//
// M2 Controller Buttons lists the shipped 3DO pad plus the vendor extended
// D/E/F buttons: the raw control-port contract (m2_cport_bits.h) and the saved
// profile tokens expose all fourteen, so the launcher must offer all fourteen.
// M2 Analog Controls holds the analog wheel and stick rows, and Host
// Shortcuts holds host-only actions. The Action enum places D/E/F after Quit,
// so the sections are explicit lists rather than contiguous enum ranges.
using m2host::input::Action;
inline bool binding_context_overlaps(Action a, Action b) {
    if (a == Action::Quit || b == Action::Quit) return true;
    const auto shooter_only = [](Action x) {
        return x == Action::ShooterFire || x == Action::ShooterBeam || x == Action::ShooterStart;
    };
    // shooter2d runs one game at a time with racing, so it only competes with
    // its own actions and the ones it shares: directions, pause, and screenshot.
    if (shooter_only(a) || shooter_only(b)) {
        const Action other = shooter_only(a) ? b : a;
        return shooter_only(other) || other == Action::DrivePause ||
               other == Action::DriveScreenshot || other == Action::DigitalUp ||
               other == Action::DigitalDown || other == Action::AnalogUp ||
               other == Action::AnalogDown || other == Action::DriveLeft ||
               other == Action::DriveRight || other == Action::DriveAnalogLeft ||
               other == Action::DriveAnalogRight;
    }
    const auto context = [](Action x) {
        return x < Action::DriveLeft ? 0 : x < Action::MenuUp || x == Action::DriveScreenshot || x >= Action::DigitalUp ? 1 : 2;
    };
    return context(a) == context(b);
}

// Controls-tab order: the recommended Custom mode comes first.
inline constexpr m2_controls_mode kControlsTabModes[] = {
    M2_CONTROLS_CUSTOM, M2_CONTROLS_ORIGINAL, M2_CONTROLS_PATCHED,
};
inline int controls_tab(m2_controls_mode mode) {
    for (size_t i = 0; i < std::size(kControlsTabModes); ++i)
        if (kControlsTabModes[i] == mode) return int(i);
    return 0;
}

inline constexpr const char* kActionLabels[] = {
    "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right",
    "A", "B", "C", "X", "L", "R", "P (Play/Pause)",
    "Analog Wheel Left", "Analog Wheel Right",
    "Analog Joystick Up", "Analog Joystick Down", "Analog Joystick Left", "Analog Joystick Right",
    "Quit game",
    "D", "E", "F",
    "Digital Left", "Digital Right",
    "Analog Left", "Analog Right", "Throttle", "Brake",
    "Shift up", "Shift down", "Cycle HUD displays",
    "Change view", "Pause",
    "Menu up", "Menu down", "Menu left", "Menu right", "Confirm (A)", "Back (B)",
    "Screenshot", "Digital Up", "Digital Down", "Analog Up", "Analog Down",
    "shooter2d: Fire", "shooter2d: Beam", "shooter2d: Start",
};
static_assert(std::size(kActionLabels) == size_t(Action::kActionCount),
              "every Action needs a Controls-tab row label");

inline constexpr Action kControllerButtonRows[] = {
    Action::DpadUp, Action::DpadDown, Action::DpadLeft, Action::DpadRight,
    Action::A, Action::B, Action::C, Action::X, Action::L, Action::R, Action::P,
    Action::D, Action::E, Action::F,
};
inline constexpr Action kAnalogRows[] = {
    Action::WheelLeft, Action::WheelRight,
    Action::StickUp, Action::StickDown, Action::StickLeft, Action::StickRight,
};
inline constexpr Action kShortcutRows[] = {Action::Quit};
inline constexpr Action kDrivingRows[] = {
    Action::AnalogUp, Action::AnalogDown, Action::DriveAnalogLeft, Action::DriveAnalogRight,
    Action::DigitalUp, Action::DigitalDown, Action::DriveLeft, Action::DriveRight,
    Action::DriveThrottle, Action::DriveBrake, Action::DriveShiftUp, Action::DriveShiftDown, Action::DriveAids,
    Action::DriveView, Action::DrivePause, Action::DriveScreenshot,
};
inline constexpr Action kMenuRows[] = {
    Action::MenuUp, Action::MenuDown, Action::MenuLeft, Action::MenuRight,
    Action::MenuConfirm, Action::MenuBack,
};
inline constexpr Action kShooterRows[] = {
    Action::ShooterFire, Action::ShooterBeam, Action::ShooterStart,
};

struct ActionRowSection {
    const char* heading;   // section title drawn above the table
    const char* column;    // first column heading
    const char* table_id;  // stable ImGui table id
    const Action* rows;
    size_t count;
};
inline constexpr ActionRowSection kActionRowSections[] = {
    {"Buttons", "Button", "m2_buttons", kControllerButtonRows,
     std::size(kControllerButtonRows)},
    {"Analog", "Control", "m2_analog", kAnalogRows,
     std::size(kAnalogRows)},
    {"Driving", "Action", "custom_driving", kDrivingRows, std::size(kDrivingRows)},
    {"Menus", "Action", "custom_menus", kMenuRows, std::size(kMenuRows)},
    {"shooter2d", "Action", "custom_shooter", kShooterRows, std::size(kShooterRows)},
    {"Shortcuts", "Action", "host_shortcuts", kShortcutRows, std::size(kShortcutRows)},
};
// Row label for an action; empty for a value outside the enum.
inline const char* action_row_label(Action action) {
    const size_t index = size_t(action);
    return index < std::size(kActionLabels) ? kActionLabels[index] : "";
}
// What each action does in the shipped game for the selected mode and layout.
inline const char* action_effect(Action action, m2_controls_mode mode,
                                 ControllerType type, int patched_preview = 1) {
    if (action == Action::DriveAids) return "Cycle HUD";
    if (action == Action::DriveScreenshot) return "Screenshot";
    if (action == Action::Quit) return "Close the game";
    if (action == Action::ShooterFire) return "Fire";
    if (action == Action::ShooterBeam) return "Beam";
    if (action == Action::ShooterStart) return "Start";
    if (action >= Action::DriveLeft) {
        if (action == Action::DriveLeft || action == Action::DriveRight)
            return "Digital steering";
        if (action == Action::DriveAnalogLeft || action == Action::DriveAnalogRight)
            return "Analog steering";
        if (action == Action::DriveThrottle) return "Analog throttle";
        if (action == Action::DriveBrake) return "Analog brake";
        if (action == Action::DriveShiftUp) return "Shift up";
        if (action == Action::DriveShiftDown) return "Shift down";
        return action >= Action::MenuUp ? "Menu control" : "";
    }
    const bool analog = mode == M2_CONTROLS_PATCHED ? patched_preview != 1 : type == ControllerType::Joystick;
    const bool wheel = mode == M2_CONTROLS_PATCHED && patched_preview == 0;
    if (action >= Action::WheelLeft && action <= Action::StickRight) {
        if (!analog) return "Does nothing";
        if (action == Action::StickUp) return "Throttle";
        if (action == Action::StickDown) return "Brake";
        return "Steering";
    }
    if (wheel) {
        switch (action) {
        case Action::DpadRight: return "Shift up";
        case Action::DpadLeft: return "Shift down";
        case Action::A: return "Confirm in menus / cycle HUD";
        case Action::B: return "Back in menus / screenshot";
        case Action::X: return "Change view";
        case Action::P: return "Pause";
        default: return "N/A";
        }
    }
    if (analog) {
        switch (action) {
        case Action::A: return "Confirm in menus / change view";
        case Action::B: return "Back in menus / cycle HUD";
        case Action::C: return "Shift down";
        case Action::X: return "Shift up";
        case Action::P: return "Pause";
        case Action::R: return "Screenshot";
        default: return "N/A";
        }
    }
    switch (action) {
    case Action::DpadUp: return "Menu up";
    case Action::DpadDown: return "Menu down";
    case Action::DpadLeft: return "Menu left / steer left";
    case Action::DpadRight: return "Menu right / steer right";
    case Action::A: return "Confirm in menus / throttle";
    case Action::B: return "Back in menus / brake";
    case Action::C: return "Cycle HUD";
    case Action::X: return "N/A";
    case Action::L: return "Shift down";
    case Action::R: return "Shift up";
    case Action::P: return "Pause";
    case Action::D: return "Screenshot";
    case Action::E: return "N/A";
    case Action::F: return "Change view";
    default: return "";
    }
}
// What each Original and Patched row does in shooter2d. The shooter tests raw pad
// bits itself, so the controller type and Patched layout do not change this.
inline const char* shooter_action_effect(Action action) {
    switch (action) {
    case Action::DpadUp:
    case Action::DpadDown:
    case Action::DpadLeft:
    case Action::DpadRight: return "Move";
    case Action::A: return "Fire";
    case Action::B: return "Beam";
    case Action::C: return "Start";
    case Action::P:
    case Action::E: return "Pause";
    case Action::R: return "Screenshot";
    case Action::L: return "Screenshot timing";
    case Action::Quit: return "Close the game";
    default: return "N/A";
    }
}
inline bool is_screenshot_action(Action action, m2_controls_mode mode,
                                  ControllerType type, int patched_preview = 1) {
    if (mode == M2_CONTROLS_CUSTOM) return action == Action::DriveScreenshot;
    if (mode == M2_CONTROLS_PATCHED && patched_preview == 0) return action == Action::B;
    const bool analog = mode == M2_CONTROLS_PATCHED ? patched_preview == 2 : type == ControllerType::Joystick;
    return action == (analog ? Action::R : Action::D);
}

inline bool is_hud_cycle_action(Action action, m2_controls_mode mode,
                                ControllerType type, int patched_preview = 1) {
    if (mode == M2_CONTROLS_CUSTOM) return action == Action::DriveAids;
    if (mode == M2_CONTROLS_PATCHED && patched_preview == 0) return action == Action::A;
    const bool analog = mode == M2_CONTROLS_PATCHED ? patched_preview == 2 : type == ControllerType::Joystick;
    return action == (analog ? Action::B : Action::C);
}

inline const char* hud_cycle_tooltip() {
    return "Labeled 'Ignition' on the original controller diagrams, but actually cycles the HUD.";
}

inline std::string screenshot_tooltip(const std::filesystem::path& directory) {
    return "Original developer debug feature. During a race, saves a 576x432 screenshot "
           "(raw screenshot.N and viewable screenshot.N.ppm) to:\n" +
           m2host::platform::path_to_utf8(directory) +
           "\nNumbering resets to 0 on each launch and overwrites existing files.";
}
// True when every Action appears in exactly one section: a missing row is
// invisible in the launcher, and a duplicated row would draw the action twice.
inline constexpr bool action_row_sections_complete() {
    bool seen[size_t(Action::kActionCount)] = {};
    for (const ActionRowSection& section : kActionRowSections)
        for (size_t i = 0; i < section.count; ++i) {
            const size_t index = size_t(section.rows[i]);
            if (index >= std::size(seen) || seen[index]) return false;
            seen[index] = true;
        }
    for (bool row : seen)
        if (!row) return false;
    return true;
}
static_assert(action_row_sections_complete(),
              "Controls-tab rows must cover every Action exactly once");

// Borderless fullscreen (a desktop-sized borderless window) is offered where
// SDL provides it on a desktop window system. The host still falls back to it
// when exclusive fullscreen is unavailable, so an unsupported platform simply
// gets the plain fullscreen option.
inline constexpr bool kBorderlessFullscreenSupported =
#if defined(SDL_PLATFORM_WINDOWS) || defined(SDL_PLATFORM_MACOS) || defined(SDL_PLATFORM_LINUX) || \
    defined(SDL_PLATFORM_FREEBSD) || defined(SDL_PLATFORM_NETBSD) || defined(SDL_PLATFORM_OPENBSD)
    true;
#else
    false;
#endif

// Launcher Graphics-tab resolution presets. Every entry is 4:3 — the guest
// display's aspect (640x480 display, 576x432 carve) — so selecting one cannot
// stretch the image; the host also locks the window's content aspect while
// playing. A saved or hand-typed size outside the list is reported as custom
// rather than silently replaced. Integer multiples of the native 640x480 are
// the pixel-exact scales and are labelled as such.
struct LauncherResolution {
    int width;
    int height;
    const char* label;
};
inline constexpr LauncherResolution kResolutionPresets[] = {
    {640, 480, "640×480 (native)"},
    {800, 600, "800×600"},
    {960, 720, "960×720"},
    {1024, 768, "1024×768"},
    {1280, 960, "1280×960 (2x)"},
    {1600, 1200, "1600×1200"},
    {1920, 1440, "1920×1440 (3x)"},
    {2560, 1920, "2560×1920 (4x)"},
};
inline constexpr int kResolutionPresetCount =
    sizeof(kResolutionPresets) / sizeof(kResolutionPresets[0]);
inline int resolution_index(int width, int height) {
    for (int i = 0; i < kResolutionPresetCount; ++i)
        if (kResolutionPresets[i].width == width &&
            kResolutionPresets[i].height == height)
            return i;
    return -1;
}
// --- Enhancements/Hacks-tab sections ------------------------------------------
struct EnhancementOption {
    const char* flag;
    const char* label;
    const char* description;
};
struct EnhancementSection {
    const char* heading;
    const EnhancementOption* options;
    size_t count;
};
inline constexpr EnhancementOption kBugFixOptions[] = {
    {"M2_SKIDPAD_ENGINE_FIX", "Skidpad engine sound fix",
     "Fixes a bug where changing the engine from the Skid Pad car configuration menu does not update the engine sound."},
    {"M2_LAGUNA_PHANTOM_FIX", "Laguna Seca phantom fix",
     "Fixes a crash in phantom mode on Laguna Seca caused by a malformed replay file."},
    {"M2_HUD_TACH_FIX", "Exterior tachometer range fix",
     "The exterior-camera tachometer is scaled for the Ferrari V12, so the other three engines never reach the gauge's yellow zone. This option rescales it from the selected engine's redline."},
};
inline constexpr EnhancementOption kRestoredFeatureOptions[] = {
    {"M2_RESTORE_CREDITS", "Restore unfinished credits routine",
     "When selecting Credits from the menu, the game plays a placeholder video. Enabling this option restores an unused, seemingly complete credits routine."},
    {"M2_RESTORE_DRAFTING", "Restore drafting",
     "Drafting code exists in the game but goes entirely unused."},
    {"M2_RESTORE_BEST_TIMES", "Restore race summary screen",
     "Restores an unused post-race screen with race time, best lap, average speed and "
     "finishing place before the high-score table. It is unknown where this screen was meant to be placed in the actual game."},
};
inline constexpr EnhancementOption kHackOptions[] = {
    {"M2_UNLOCK_SECRET_TRACK", "Unlock secret track",
     "Unlocks the secret Psychedelia track in the track selection menu without having to trigger the unlock on the Skid Pad."},
};
inline constexpr EnhancementSection kEnhancementSections[] = {
    {"Bug Fixes", kBugFixOptions, std::size(kBugFixOptions)},
    {"Restored Features", kRestoredFeatureOptions, std::size(kRestoredFeatureOptions)},
    {"Hacks", kHackOptions, std::size(kHackOptions)},
};
inline constexpr const char* kCarVariantOptionLabel = "Player car livery variant";
inline constexpr const char* kCarVariantOptionDescription =
    "Each car has three liveries, two of which are normally only used by AI opponents. Applies to races only, not the showroom or car select screen.";

// Flags with a dedicated launcher control. The launcher merges and exports
// only these rows; other rows in launcher.cfg (added by hand) are kept but do
// not reach the game's environment. The generated catalog remains the
// reference list for names:
// the settings test asserts every entry here is a real catalog row, so a flag
// rename cannot leave a dead option behind.
inline constexpr const char* kLauncherFlagNames[] = {
    "M2_IMPROVED_ANALOG_STEERING", // Controls: optional grip-aware steering
    "M2_ANISOTROPY",    // Graphics: anisotropic filtering
    "M2_ANTIALIASING",  // Graphics: MSAA
    "M2_RENDER_SCALE",  // Graphics: internal resolution
    "M2_TEXTURE_FILTER",   // Graphics: texture filtering
    "M2_BITMAP_FILTER",    // Graphics: menu/FMV bitmap filtering
    "M2_HIRES_EFFECTS",    // Graphics: full-resolution feedback effects
    "M2_DISABLE_CAR_LOD",  // Graphics: opt-in highest-detail car models
    "M2_RGB555_DITHER",  // Graphics: opt-in RGB555 output dithering
    "M2_PLAYER_CAR_VARIANT",  // Enhancements: player-car body variant
    "M2_SKIDPAD_ENGINE_FIX",  // Enhancements: skidpad engine-sound rebind
    "M2_RESTORE_CREDITS",     // Enhancements: restore unfinished credits
    "M2_RESTORE_BEST_TIMES",  // Enhancements: restore post-race summary
    "M2_RESTORE_DRAFTING",    // Enhancements: restore drafting
    "M2_UNLOCK_SECRET_TRACK", // Enhancements: unlock secret track
    "M2_LAGUNA_PHANTOM_FIX",  // Enhancements: default-on Laguna phantom fix
    "M2_HUD_TACH_FIX",        // Enhancements: default-on exterior tach range fix
    "M2_CROP_OVERSCAN",       // Graphics: default-on crop of the built-in overscan
};
// Default-on fixes: the game applies them unless the variable is exactly "0".
inline constexpr const char* kDefaultOnLauncherFlags[] = {
    "M2_LAGUNA_PHANTOM_FIX",
    "M2_HUD_TACH_FIX",
    "M2_CROP_OVERSCAN",
};
inline constexpr int kLauncherFlagCount =
    int(sizeof(kLauncherFlagNames) / sizeof(kLauncherFlagNames[0]));
inline bool is_launcher_flag(const char* name) {
    const std::string want(name);
    for (const char* candidate : kLauncherFlagNames)
        if (want == candidate) return true;
    return false;
}
// Merge the curated rows into loaded (or default) settings: file rows win
// (values preserved), missing names receive their launcher defaults.
inline void ensure_launcher_flags(Settings& s) {
    for (const char* name : kLauncherFlagNames) {
        if (find_flag(s, name) >= 0) continue;
        if (s.flags.size() >= 512) return;
        RunFlag flag; copy(flag.name, name);
        if (std::string(name) == "M2_IMPROVED_ANALOG_STEERING" ||
            std::string(name) == "M2_SKIDPAD_ENGINE_FIX") {
            flag.enabled = true;
            copy(flag.value, "1");
        }
        s.flags.push_back(flag);
    }
}
// Curated opt-in boolean option. The flag's own contract treats the exact
// value "1" as on and every other value as off, so a saved 0 or hand-typed
// value reads as unchecked. Turning the option off clears the value, leaving
// the variable unset in the child and restoring original behavior.
inline bool flag_is_on(const Settings& s, const char* name) {
    const int row = find_flag(s, name);
    return row >= 0 && s.flags[size_t(row)].enabled &&
           std::string(s.flags[size_t(row)].value.data()) == "1";
}
inline void set_flag_on(Settings& s, const char* name, bool on) {
    const int row = find_flag(s, name);
    if (row < 0) return;
    auto& flag = s.flags[size_t(row)];
    flag.enabled = on;
    copy(flag.value, on ? "1" : "");
}
inline bool is_default_on_flag(const char* name) {
    const std::string want(name);
    for (const char* candidate : kDefaultOnLauncherFlags)
        if (want == candidate) return true;
    return false;
}
// Default-on options read as on unless the row exports exactly "0". Turning
// one on clears the row, leaving the variable unset so the game applies its
// default; turning it off exports "0", the only value that disables it.
inline bool default_on_flag_is_on(const Settings& s, const char* name) {
    const int row = find_flag(s, name);
    return row < 0 || !s.flags[size_t(row)].enabled ||
           std::string(s.flags[size_t(row)].value.data()) != "0";
}
inline void set_default_on_flag(Settings& s, const char* name, bool on) {
    const int row = find_flag(s, name);
    if (row < 0) return;
    auto& flag = s.flags[size_t(row)];
    flag.enabled = !on;
    copy(flag.value, on ? "" : "0");
}
// One check box per curated option, whichever default its flag has.
inline bool option_is_on(const Settings& s, const char* name) {
    return is_default_on_flag(name) ? default_on_flag_is_on(s, name)
                                    : flag_is_on(s, name);
}
inline void set_option_on(Settings& s, const char* name, bool on) {
    if (is_default_on_flag(name)) set_default_on_flag(s, name, on);
    else set_flag_on(s, name, on);
}
}  // namespace launcher_flags
