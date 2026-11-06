#pragma once
#include <SDL3/SDL.h>
#include <array>
#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "input_map.h"

namespace m2host::input {
// Physical inputs are persisted separately from guest-facing replay records.
struct HostBinding {
    enum Kind { Key, MouseButton, MouseWheel, MouseMotion, JoyButton, JoyHat, JoyAxis };
    Kind kind = Key;
    std::string device; // SDL GUID + serial when available; never an instance ID
    int id = 0;
    int direction = 1;
    float rest = 0.f;
    float endpoint = 1.f;
    float deadzone = 0.08f;
};
struct HostControl {
    Action action = Action::DpadUp;
    HostBinding binding;
};
// True when both bindings name the same physical input (ignoring calibration
// and dead zone). Used to offer moving an input that another action already has.
bool same_input(const HostBinding& a, const HostBinding& b);
class HostControls {
public:
    static constexpr size_t kMaxBindings = 1024;
    HostControls() = default;
    ~HostControls();
    HostControls(const HostControls&) = delete;
    HostControls& operator=(const HostControls&) = delete;
    std::vector<HostControl> bindings;
    void defaults();
    void custom_defaults();
    bool load(const std::filesystem::path& path, std::string& error);
    bool save(const std::filesystem::path& path, std::string& error) const;
    void open_devices();
    void close_devices();
    void event(const SDL_Event& event);
    void reset();
    std::array<float, size_t(Action::kActionCount)> levels() const;
    void end_frame();
    void start_capture();
    bool capture(const SDL_Event& event, HostBinding& result);
    // Binds `binding` to `action`, replacing every previous binding the action
    // had. Other actions are untouched.
    void set_binding(Action action, const HostBinding& binding);
    void clear_bindings(Action action);
    std::string describe(const HostBinding& binding) const;
    std::vector<std::string> devices() const;
private:
    struct Device { SDL_Joystick* joystick; std::string key; std::string name; };
    std::map<SDL_JoystickID, Device> devices_;
    std::map<std::pair<SDL_JoystickID, int>, float> capture_rest_;
    std::vector<float> values_;
    void add_device(SDL_JoystickID id);
    std::string device_key(SDL_JoystickID id) const;
};
std::filesystem::path launcher_settings_directory();
}
