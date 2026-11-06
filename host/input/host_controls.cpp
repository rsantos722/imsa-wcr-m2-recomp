#include "host_controls.h"
#include "platform.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace m2host::input {
std::filesystem::path launcher_settings_directory() {
#if !defined(__APPLE__)
    // A portable release package keeps everything in its own folder.
    const std::filesystem::path root = m2host::platform::portable_root(m2host::platform::executable_dir());
    if (!root.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(root / "userdata", ec);
        return root / "userdata";
    }
#endif
    char* p = SDL_GetPrefPath("", "imsa-wcr-m2-recomp");
    // SDL returns UTF-8; the narrow path constructor is the ANSI code page on Windows.
    std::filesystem::path result = p ? m2host::platform::path_from_utf8(p) : std::filesystem::path{};
    SDL_free(p);
    return result;
}
HostControls::~HostControls() { close_devices(); }
void HostControls::custom_defaults() {
    bindings.erase(std::remove_if(bindings.begin(), bindings.end(), [](const HostControl& c) {
        return c.action >= Action::DriveLeft;
    }), bindings.end());
    const std::pair<Action, SDL_Scancode> keys[] = {
        {Action::DriveLeft, SDL_SCANCODE_LEFT}, {Action::DriveRight, SDL_SCANCODE_RIGHT},
        {Action::DriveThrottle, SDL_SCANCODE_X}, {Action::DriveBrake, SDL_SCANCODE_Z},
        {Action::DriveShiftUp, SDL_SCANCODE_LSHIFT}, {Action::DriveShiftDown, SDL_SCANCODE_RSHIFT},
        {Action::DriveAids, SDL_SCANCODE_V},
        {Action::DriveView, SDL_SCANCODE_C}, {Action::DrivePause, SDL_SCANCODE_P},
        {Action::DigitalUp, SDL_SCANCODE_UP}, {Action::DigitalDown, SDL_SCANCODE_DOWN},
        {Action::MenuConfirm, SDL_SCANCODE_X}, {Action::MenuBack, SDL_SCANCODE_Z},
        {Action::ShooterFire, SDL_SCANCODE_X}, {Action::ShooterBeam, SDL_SCANCODE_Z},
        {Action::ShooterStart, SDL_SCANCODE_RETURN},
    };
    for (const auto& [action, key] : keys) { HostBinding b; b.id = key; bindings.push_back({action, b}); }
    reset();
}
void HostControls::close_devices() {
    for (auto& [id, d] : devices_) SDL_CloseJoystick(d.joystick);
    devices_.clear();
}
void HostControls::defaults() {
    bindings.clear();
    const std::pair<Action, SDL_Scancode> default_keys[] = {
        {Action::DpadUp, SDL_SCANCODE_UP},
        {Action::DpadDown, SDL_SCANCODE_DOWN},
        {Action::DpadLeft, SDL_SCANCODE_LEFT},
        {Action::DpadRight, SDL_SCANCODE_RIGHT},
        {Action::A, SDL_SCANCODE_X},
        {Action::B, SDL_SCANCODE_Z},
        {Action::C, SDL_SCANCODE_V},
        {Action::X, SDL_SCANCODE_C},
        {Action::L, SDL_SCANCODE_LSHIFT},
        {Action::R, SDL_SCANCODE_RSHIFT},
        {Action::P, SDL_SCANCODE_ESCAPE},
        {Action::Quit, SDL_SCANCODE_Q},
        // Vendor extended pad buttons.
        {Action::D, SDL_SCANCODE_B},
        {Action::E, SDL_SCANCODE_N},
        {Action::F, SDL_SCANCODE_M},
    };
    for (const auto& [action, scancode] : default_keys) {
        HostBinding b; b.id = scancode;
        bindings.push_back({action, b});
    }
    custom_defaults();
    reset();
}
bool same_input(const HostBinding& a, const HostBinding& b) {
    if (a.kind != b.kind || a.id != b.id) return false;
    switch (a.kind) {
    case HostBinding::Key:
    case HostBinding::MouseButton:
        return true;
    case HostBinding::MouseWheel:
    case HostBinding::MouseMotion:
        return a.direction == b.direction;
    case HostBinding::JoyButton:
    case HostBinding::JoyHat:
        return a.device == b.device;
    case HostBinding::JoyAxis:
        return a.device == b.device && (a.endpoint > a.rest) == (b.endpoint > b.rest);
    }
    return false;
}
void HostControls::set_binding(Action action, const HostBinding& binding) {
    bindings.erase(std::remove_if(bindings.begin(), bindings.end(),
                                  [&](const HostControl& control) {
                                      return control.action == action;
                                  }),
                   bindings.end());
    bindings.push_back({action, binding});
    reset();
}
void HostControls::clear_bindings(Action action) {
    bindings.erase(std::remove_if(bindings.begin(), bindings.end(),
                                  [&](const HostControl& control) {
                                      return control.action == action;
                                  }),
                   bindings.end());
    reset();
}
bool HostControls::load(const std::filesystem::path& path, std::string& error) {
    std::ifstream f(path);
    std::string magic; int version = 0;
    if (!(f >> magic >> version) || magic != "m2-host-controls" || (version < 1 || version > 4)) {
        error = "Cannot read controls profile (expected version 1 to 4): " + m2host::platform::path_to_utf8(path); return false;
    }
    std::vector<HostControl> parsed;
    std::string token;
    while (f >> token) {
        HostControl c; int kind;
        auto& b = c.binding;
        if (!action_from_name(token.c_str(), &c.action) ||
            !(f >> kind >> std::quoted(b.device) >> b.id >> b.direction >> b.rest >> b.endpoint >> b.deadzone) ||
            kind < HostBinding::Key || kind > HostBinding::JoyAxis || b.id < 0 || b.id > 65535 ||
            (b.direction != -1 && b.direction != 1) || !std::isfinite(b.rest) ||
            !std::isfinite(b.endpoint) || !std::isfinite(b.deadzone) || b.deadzone < 0 || b.deadzone >= 1 ||
            b.rest < -1 || b.rest > 1 || b.endpoint < -1 || b.endpoint > 1 ||
            (kind == HostBinding::JoyAxis && std::fabs(b.endpoint - b.rest) < 0.1f) || parsed.size() >= kMaxBindings) {
            error = "Invalid control binding in " + m2host::platform::path_to_utf8(path); return false;
        }
        b.kind = HostBinding::Kind(kind);
        // Version 1 had one steering pair and split it by input kind; its axis
        // and mouse-motion steering moves to the separate analog pair.
        if (version == 1 && (b.kind == HostBinding::JoyAxis || b.kind == HostBinding::MouseMotion)) {
            if (c.action == Action::DriveLeft) c.action = Action::DriveAnalogLeft;
            else if (c.action == Action::DriveRight) c.action = Action::DriveAnalogRight;
        }
        if (version < 3 && c.action >= Action::MenuUp && c.action <= Action::MenuRight) {
            const bool analog = b.kind == HostBinding::JoyAxis || b.kind == HostBinding::MouseMotion;
            switch (c.action) {
            case Action::MenuUp: c.action = analog ? Action::AnalogUp : Action::DigitalUp; break;
            case Action::MenuDown: c.action = analog ? Action::AnalogDown : Action::DigitalDown; break;
            case Action::MenuLeft: c.action = analog ? Action::DriveAnalogLeft : Action::DriveLeft; break;
            case Action::MenuRight: c.action = analog ? Action::DriveAnalogRight : Action::DriveRight; break;
            default: break;
            }
        }
        const bool directional = c.action == Action::DriveLeft || c.action == Action::DriveRight ||
            c.action == Action::DriveAnalogLeft || c.action == Action::DriveAnalogRight ||
            c.action == Action::DigitalUp || c.action == Action::DigitalDown ||
            c.action == Action::AnalogUp || c.action == Action::AnalogDown;
        const bool duplicate = version < 3 && directional && std::any_of(parsed.begin(), parsed.end(), [&](const HostControl& existing) {
            return existing.action == c.action && same_input(existing.binding, c.binding);
        });
        if (!duplicate) parsed.push_back(c);
    }
    if (!f.eof()) { error = "Cannot read " + m2host::platform::path_to_utf8(path); return false; }
    // Before version 4 the shooter read these racing actions. Copy their
    // bindings so an existing profile plays the shooter the same way.
    if (version < 4) {
        const size_t count = parsed.size();
        for (size_t i = 0; i < count && parsed.size() < kMaxBindings; ++i) {
            HostControl copy = parsed[i];
            if (copy.action == Action::MenuConfirm) copy.action = Action::ShooterFire;
            else if (copy.action == Action::MenuBack) copy.action = Action::ShooterBeam;
            else if (copy.action == Action::DriveAids) copy.action = Action::ShooterStart;
            else continue;
            parsed.push_back(copy);
        }
    }
    bindings = std::move(parsed);
    reset(); error.clear(); return true;
}
bool HostControls::save(const std::filesystem::path& path, std::string& error) const {
    if (bindings.size() > kMaxBindings) { error = "Too many control bindings"; return false; }
    // SDL rename replaces existing files on supported platforms. Write alongside
    // the destination so failure never truncates a user's previous profile.
    auto pending = path; pending += ".new";
    std::ofstream f(pending, std::ios::trunc);
    f << "m2-host-controls 4\n" << std::setprecision(9);
    for (const auto& c : bindings) {
        const auto& b = c.binding;
        f << action_name(c.action) << ' ' << int(b.kind) << ' ' << std::quoted(b.device)
          << ' ' << b.id << ' ' << b.direction << ' ' << b.rest << ' ' << b.endpoint << ' ' << b.deadzone << '\n';
    }
    f.close();
    if (!f || !SDL_RenamePath(m2host::platform::path_to_utf8(pending).c_str(), m2host::platform::path_to_utf8(path).c_str())) {
        error = "Could not save " + m2host::platform::path_to_utf8(path);
        SDL_RemovePath(m2host::platform::path_to_utf8(pending).c_str()); return false;
    }
    error.clear(); return true;
}
void HostControls::add_device(SDL_JoystickID id) {
    if (devices_.count(id)) return;
    SDL_Joystick* j = SDL_OpenJoystick(id);
    if (!j) return;
    char guid[33]; SDL_GUIDToString(SDL_GetJoystickGUID(j), guid, sizeof(guid));
    std::string key = guid;
    const char* serial = SDL_GetJoystickSerial(j);
    if (serial && *serial) key += ":" + std::string(serial);
    const char* name = SDL_GetJoystickName(j);
    devices_.emplace(id, Device{j, key, name ? name : "Joystick"});
}
void HostControls::open_devices() {
    int n = 0; SDL_JoystickID* ids = SDL_GetJoysticks(&n);
    for (int i = 0; i < n; ++i) add_device(ids[i]);
    SDL_free(ids);
}
std::string HostControls::device_key(SDL_JoystickID id) const {
    auto it = devices_.find(id); return it == devices_.end() ? "" : it->second.key;
}
void HostControls::reset() { values_.assign(bindings.size(), 0.f); }
void HostControls::event(const SDL_Event& e) {
    if (e.type == SDL_EVENT_JOYSTICK_ADDED) { add_device(e.jdevice.which); return; }
    if (e.type == SDL_EVENT_JOYSTICK_REMOVED) {
        auto it = devices_.find(e.jdevice.which);
        if (it != devices_.end()) { SDL_CloseJoystick(it->second.joystick); devices_.erase(it); }
        reset(); return;
    }
    if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) { reset(); return; }
    if (values_.size() != bindings.size()) reset();
    for (size_t i = 0; i < bindings.size(); ++i) {
        const auto& b = bindings[i].binding;
        switch (b.kind) {
        case HostBinding::Key:
            if ((e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) && e.key.scancode == b.id)
                values_[i] = e.type == SDL_EVENT_KEY_DOWN ? 1.f : 0.f;
            break;
        case HostBinding::MouseButton:
            if ((e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) && e.button.button == b.id)
                values_[i] = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 1.f : 0.f;
            break;
        case HostBinding::MouseWheel:
            if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                float v = b.id == 0 ? e.wheel.x : e.wheel.y;
                if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) v = -v;
                values_[i] = std::clamp(v * b.direction, 0.f, 1.f);
            }
            break;
        case HostBinding::MouseMotion:
            if (e.type == SDL_EVENT_MOUSE_MOTION)
                values_[i] = std::clamp((b.id == 0 ? e.motion.xrel : e.motion.yrel) * b.direction / 30.f, 0.f, 1.f);
            break;
        case HostBinding::JoyButton:
            if ((e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN || e.type == SDL_EVENT_JOYSTICK_BUTTON_UP) &&
                e.jbutton.button == b.id && device_key(e.jbutton.which) == b.device)
                values_[i] = e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN ? 1.f : 0.f;
            break;
        case HostBinding::JoyHat:
            if (e.type == SDL_EVENT_JOYSTICK_HAT_MOTION && e.jhat.hat == b.id / 16 && device_key(e.jhat.which) == b.device)
                values_[i] = (e.jhat.value & (b.id % 16)) != 0 ? 1.f : 0.f;
            break;
        case HostBinding::JoyAxis:
            if (e.type == SDL_EVENT_JOYSTICK_AXIS_MOTION && e.jaxis.axis == b.id && device_key(e.jaxis.which) == b.device) {
                const float raw = e.jaxis.value < 0 ? e.jaxis.value / 32768.f : e.jaxis.value / 32767.f;
                float v = std::clamp((raw - b.rest) / (b.endpoint - b.rest), 0.f, 1.f);
                values_[i] = std::max(0.f, (v - b.deadzone) / (1.f - b.deadzone));
            }
            break;
        }
    }
}
std::array<float, size_t(Action::kActionCount)> HostControls::levels() const {
    std::array<float, size_t(Action::kActionCount)> result{};
    for (size_t i = 0; i < bindings.size() && i < values_.size(); ++i)
        result[size_t(bindings[i].action)] = std::max(result[size_t(bindings[i].action)], values_[i]);
    return result;
}
void HostControls::end_frame() {
    for (size_t i = 0; i < bindings.size() && i < values_.size(); ++i)
        if (bindings[i].binding.kind == HostBinding::MouseWheel || bindings[i].binding.kind == HostBinding::MouseMotion) values_[i] = 0;
}
void HostControls::start_capture() {
    capture_rest_.clear();
    for (const auto& [id, d] : devices_)
        for (int a = 0; a < SDL_GetNumJoystickAxes(d.joystick); ++a) {
            const auto v = SDL_GetJoystickAxis(d.joystick, a);
            capture_rest_[{id, a}] = v < 0 ? v / 32768.f : v / 32767.f;
        }
}
bool HostControls::capture(const SDL_Event& e, HostBinding& b) {
    b = {};
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) { b.id = e.key.scancode; return true; }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) { b.kind = HostBinding::MouseButton; b.id = e.button.button; return true; }
    if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        b.kind = HostBinding::MouseWheel; b.id = std::fabs(e.wheel.x) > std::fabs(e.wheel.y) ? 0 : 1;
        float v = b.id == 0 ? e.wheel.x : e.wheel.y;
        if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) v = -v;
        b.direction = v < 0 ? -1 : 1; return v != 0;
    }
    // Mouse motion is not a capture input; it is only kept for profiles that
    // already store a mouse-motion binding.
    SDL_JoystickID id = 0;
    if (e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN) { b.kind = HostBinding::JoyButton; b.id = e.jbutton.button; id = e.jbutton.which; }
    else if (e.type == SDL_EVENT_JOYSTICK_HAT_MOTION && e.jhat.value) {
        b.kind = HostBinding::JoyHat; id = e.jhat.which;
        int direction = 1; while (!(e.jhat.value & direction)) direction <<= 1;
        b.id = e.jhat.hat * 16 + direction;
    } else if (e.type == SDL_EVENT_JOYSTICK_AXIS_MOTION) {
        b.kind = HostBinding::JoyAxis; id = e.jaxis.which; b.id = e.jaxis.axis;
        auto rest = capture_rest_.find({id, b.id}); if (rest == capture_rest_.end()) return false;
        b.rest = rest->second;
        const float v = e.jaxis.value < 0 ? e.jaxis.value / 32768.f : e.jaxis.value / 32767.f;
        if (std::fabs(v - b.rest) < 0.5f) return false;
        b.endpoint = v > b.rest ? 1.f : -1.f;
    } else return false;
    b.device = device_key(id); return !b.device.empty();
}
std::string HostControls::describe(const HostBinding& b) const {
    if (b.kind == HostBinding::Key) { const char* name = SDL_GetScancodeName(SDL_Scancode(b.id)); return std::string("Key: ") + (name && *name ? name : "Unknown"); }
    const char* names[] = {"Key", "Mouse button", "Mouse wheel", "Mouse movement", "Button", "Hat", "Axis"};
    std::string prefix;
    if (!b.device.empty()) {
        prefix = "Disconnected device";
        for (const auto& [id, d] : devices_) if (d.key == b.device) { prefix = d.name; break; }
        prefix += " / ";
    }
    std::string result = prefix + names[b.kind] + " " + std::to_string(b.id);
    if (b.kind == HostBinding::JoyAxis) result += b.endpoint > b.rest ? " +" : " -";
    if (b.kind == HostBinding::MouseWheel || b.kind == HostBinding::MouseMotion) result += b.direction > 0 ? " +" : " -";
    return result;
}
std::vector<std::string> HostControls::devices() const {
    std::vector<std::string> result;
    for (const auto& [id, d] : devices_) result.push_back(d.name + " [" + d.key + "]");
    return result;
}
}
