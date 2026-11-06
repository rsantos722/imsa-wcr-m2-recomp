// input_map.h — host input subsystem: per-action levels -> CONTROLS.c port.
//
// HostControls (host_controls.h) turns physical SDL input into one level per
// Action. InputMap turns those levels into the control-port snapshot that
// recompiled game code polls each frame, shaped to match what
// controls.c:GetCPortData hands to Ctrl_TranslateInput (buttons word, edge
// masks and raw axes; see docs/game/state-machine.md and the
// FN_GetCPortData__0f9a8 lift).
//
// Original keeps guest-owned action interpretation. Custom supplies explicit
// user-bound input actions; driving aids and simulation remain guest-owned.
// Custom steering has separate digital and analog actions so ProcessInput can
// run the guest's digital pad ramp and arbitrate it against the analog command.
//
// Deliberately SDL-header-free so the module and its selftest run headless.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace m2host::input {

// --- logical actions / controller inputs -------------------------------------
// M2 controller buttons, analog controls, and host shortcuts.
enum class Action : uint8_t {
    // M2 Controller Buttons & D-Pad
    DpadUp = 0,
    DpadDown,
    DpadLeft,
    DpadRight,
    A,
    B,
    C,
    X,
    L,
    R,
    P,

    // M2 Analog Controls
    WheelLeft,
    WheelRight,
    StickUp,
    StickDown,
    StickLeft,
    StickRight,

    // Host Shortcuts
    Quit,

    // Extended pad buttons.
    D,
    E,
    F,

    // Direct IMSA actions used only by the launcher's Custom mode. Profiles
    // persist the token names (action_name), never these numeric positions.
    // DriveAids (`drive_aids`) is Cycle HUD displays.
    // DriveShiftUp/Down emit the shipped shift+clutch words (0x41/0x42);
    // there is no independent clutch action.
    // DriveLeft/Right feed the digital-pad steering ramp whatever input is
    // bound; DriveAnalogLeft/Right feed the absolute analog command.
    DriveLeft, DriveRight, DriveAnalogLeft, DriveAnalogRight, DriveThrottle, DriveBrake,
    DriveShiftUp, DriveShiftDown, DriveAids,
    DriveView, DrivePause,
    MenuUp, MenuDown, MenuLeft, MenuRight, MenuConfirm, MenuBack,
    DriveScreenshot,
    DigitalUp, DigitalDown, AnalogUp, AnalogDown,

    // Custom actions that only shooter2d reads. It shares the direction, pause,
    // and screenshot actions with racing.
    ShooterFire, ShooterBeam, ShooterStart,

    kActionCount,
};

const char* action_name(Action a);          // stable controls-profile token
bool action_from_name(const char* tok, Action* out);

// Canonical host-side button bits for ControlPortState.buttons. The recomp
// bridge maps them to the raw 3DO ControlPort word
// (`runtime/include/m2_cport_bits.h`), which the lifted guest translators
// consume; that mapping follows the shipped guest map table. Ordering follows
// the standard 3DO pad convention (A/B/C/P).
enum CportBits : uint32_t {
    kBtnA = 1u << 0,
    kBtnB = 1u << 1,
    kBtnC = 1u << 2,
    kBtnStart = 1u << 3,
    kBtnL = 1u << 4,     // shoulder / shift-down side
    kBtnR = 1u << 5,     // shoulder / shift-up side
    kBtnX = 1u << 6,
    // Bit 7 is unused.
    kBtnDpadUp = 1u << 8,
    kBtnDpadDown = 1u << 9,
    kBtnDpadLeft = 1u << 10,
    kBtnDpadRight = 1u << 11,
    kBtnD = 1u << 12,
    kBtnE = 1u << 13,
    kBtnF = 1u << 14,
};

// --- control-port snapshot -----------------------------------------------------
// Host-side equivalent of the record GetCPortData fills for Ctrl_TranslateInput.
//
// The axes are what the DRIVER LAYER publishes to the guest. The vendor
// `StickDriver.c` (drivers/eventbroker) decodes the hardware bytes into 10-bit
// X/Y/Z positions and republishes them verbatim in `StickEventData`: no
// deadzone, saturation, gamma, or any other curve. Its only shaping is
// `FUZZ_VAL 1`, which merely sets `EVENTBIT0_StickMoved`. The guest owns
// calibration: `GetCPortData -> FilterStick -> FilterControl` is the original
// self-calibrating stage (min/max envelope tracker), and `GetCPortData` then
// applies its own fctiwz+clamp. The host therefore never shapes these values;
// the only host-side shaping is each binding's dead zone in HostControls.
//
// See docs/game/physics-and-aero.md for the guest input modes.
struct ControlPortState {
    uint32_t buttons = 0;
    uint32_t pressed = 0;    // rising edges since begin_frame()
    uint32_t released = 0;   // falling edges since begin_frame()

    // Device travel from the analog Wheel/Stick actions. The digital pad
    // consumes only the button word.
    float raw_steer = 0.f;     // -1..+1 (right positive)
    float raw_throttle = 0.f;  // 0..1
    float raw_brake = 0.f;     // 0..1

    uint8_t stick_x = 128;   // packed axis form: 128 = center
    uint8_t stick_y = 128;
    uint8_t device_index = 0;

    // Custom-mode action channels, separate from the original Stick protocol.
    // Digital and analog steering stay separate so the guest can run its own
    // ramped digital-pad steering alongside the absolute analog command.
    float custom_steer_analog = 0.f;  // DriveAnalogLeft/Right, right positive
    float custom_steer_digital = 0.f; // DriveLeft/Right, right positive
    float custom_throttle = 0.f, custom_brake = 0.f;

    // Derive stick_x/stick_y from RAW axes without host calibration.
    void pack_bytes();
};

float axis_to_byte(float v);  // -1..+1 → 0..255, 0 → 128 (round-half-away)

// --- level -> port mapping ----------------------------------------------------
class InputMap {
public:
    static constexpr int kMaxPads = 4;

    using Levels = std::array<float, std::size_t(Action::kActionCount)>;

    void begin_frame();                 // clear pressed/released edges
    // Publish one level per action (0..1) and rebuild port 0.
    void set_live_levels(const Levels& levels);
    // Rebuild port 0 from the current levels and record it (end of frame).
    void tick();
    void set_custom_controls(bool enabled) { m_custom_controls = enabled; }
    // Select shooter2d's Custom button mapping. The disc shell switches games
    // mid-session, so this follows the running guest rather than launch options.
    void set_shooter_active(bool active) { m_shooter_active = active; }

    const ControlPortState& control_port(uint8_t idx) const;  // 0..3
    bool quit_requested() const { return m_quit; }

    // Replay injection — directly overwrite port 0 from a recorded InputRecord
    // without going through levels. Used when --replay-input is active.
    void inject_replay(uint32_t buttons, uint32_t pressed, uint32_t released,
                       uint8_t stick_x, uint8_t stick_y, uint8_t device);

private:
    void rebuild_port();  // levels -> ControlPortState (buttons, axes, edges)

    // Per-action analog/digital level, 0..1 (steering uses signed pairs of
    // actions).
    Levels m_level{};

    ControlPortState m_ports[kMaxPads];
    uint32_t m_prev_buttons = 0;  // edge-detection baseline for port 0
    bool m_quit = false;
    bool m_custom_controls = false;
    bool m_shooter_active = false;
};

}  // namespace m2host::input
