#include "input_map.h"
#include <cstring>
namespace m2host::input {
// --- action names --------------------------------------------------------------
static const char* const kActionNames[size_t(Action::kActionCount)] = {
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "a", "b", "c", "x", "l", "r", "p",
    "wheel_left", "wheel_right",
    "stick_up", "stick_down", "stick_left", "stick_right",
    "quit", "d", "e", "f",
    "drive_left", "drive_right", "drive_analog_left", "drive_analog_right", "drive_throttle", "drive_brake",
    "drive_shift_up", "drive_shift_down", "drive_aids",
    "drive_view", "drive_pause",
    "menu_up", "menu_down", "menu_left", "menu_right", "menu_confirm", "menu_back",
    "drive_screenshot",
    "digital_up", "digital_down", "analog_up", "analog_down",
    "shooter_fire", "shooter_beam", "shooter_start",
};

const char* action_name(Action a) {
    const size_t i = size_t(a);
    return i < size_t(Action::kActionCount) ? kActionNames[i] : "?";
}

bool action_from_name(const char* tok, Action* out) {
    if (!tok || !out) return false;
    for (size_t i = 0; i < size_t(Action::kActionCount); ++i) {
        if (std::strcmp(tok, kActionNames[i]) == 0) {
            *out = static_cast<Action>(i);
            return true;
        }
    }
    return false;
}

}
