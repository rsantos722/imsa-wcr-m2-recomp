// input_map.cpp — implementation. See input_map.h for the design;
// Guest side: controls.c GetCPortData record shape and FilterStick
// calibration (docs/game/state-machine.md, docs/game/physics-and-aero.md).
// Gameplay interpretation belongs to the guest.
#include "input_map.h"

#include <utility>

#include "input_recorder.h"
#ifdef M2_HAVE_RECOMP
#include "recomp_helpers.h"
#endif

namespace m2host::input {

// --- control-port packing ---------------------------------------------------------
float axis_to_byte(float v) {
    // -1..+1 -> 0..255 with 128 at rest; round-half-away mirrors the guest's
    // fctiwz+clamp path (FN_GetCPortData__0f9a8 0xfb50..0xfb98).
    float s = v * 127.f;
    s = s < 0.f ? s - 0.5f : s + 0.5f;
    int i = int(s);
    if (i < -127) i = -127;
    if (i > 127) i = 127;
    return float(i + 128);
}

// Raw-travel clamps. The guest-bound value stays a faithful device
// publication (vendor `StickDriver.c` publishes the hardware position
// verbatim).
static float clamp_unit(float v) {
    return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}

static float clamp_sym(float v) {
    return v < -1.f ? -1.f : (v > 1.f ? 1.f : v);
}

void ControlPortState::pack_bytes() {
    // Guest stores two axis bytes after its own conversion. Derive them from
    // the RAW device travel: the vendor `StickDriver.c` publishes unshaped
    // 10-bit positions and the guest's `FilterStick`/`FilterControl` is the
    // original (self-calibrating) filter, so shaping the travel here would
    // filter the same signal twice and would irreversibly zero sub-deadzone
    // deflection.
    //
    // The pedal merge (Y = brake − throttle) is NOT a host simplification: it
    // is the guest's own mode-7 wire protocol. ProcessInput@0x9680 splits the
    // byte by sign (v = byte*c − 1; v>0 → brake [out+0x10], else throttle
    // [out+0xc]) at 0x981c..0x985c, so independent pedal data cannot survive
    // the pipe. The driver's three independent 10-bit positions are consumed
    // only by the mode3/4 nibble protocol (ProcessInput 0x98bc..0x990c, two
    // 4-bit channels in one byte), which the live state-3 record does not
    // select — the mode byte is guest data. See the input modes in
    // docs/game/physics-and-aero.md.
    stick_x = uint8_t(axis_to_byte(raw_steer) + 0.5f);
    stick_y = uint8_t(axis_to_byte(raw_brake - raw_throttle) + 0.5f);
}

// --- InputMap ----------------------------------------------------------------------
namespace {
// Action -> canonical control-port button bit.
uint32_t action_button_bit(Action a) {
    switch (a) {
    case Action::DpadLeft: return kBtnDpadLeft;
    case Action::DpadRight: return kBtnDpadRight;
    case Action::DpadUp: return kBtnDpadUp;
    case Action::DpadDown: return kBtnDpadDown;
    case Action::R: return kBtnR;
    case Action::L: return kBtnL;
    case Action::A: return kBtnA;
    case Action::B: return kBtnB;
    case Action::C: return kBtnC;
    case Action::X: return kBtnX;
    case Action::P: return kBtnStart;
    case Action::D: return kBtnD;
    case Action::E: return kBtnE;
    case Action::F: return kBtnF;
    default: return 0;
    }
}
}  // namespace

const ControlPortState& InputMap::control_port(uint8_t idx) const {
    return m_ports[idx % kMaxPads];
}

void InputMap::begin_frame() {
    for (auto& p : m_ports) {
        p.pressed = 0;
        p.released = 0;
    }
}

void InputMap::rebuild_port() {
    ControlPortState& p = m_ports[0];

    // Levels are raw directed travel: the driver-layer publication the guest
    // consumes.
    const float wheel_steer = m_level[size_t(Action::WheelRight)] -
                              m_level[size_t(Action::WheelLeft)];
    const float stick_steer = m_level[size_t(Action::StickRight)] -
                              m_level[size_t(Action::StickLeft)];
    p.raw_steer = clamp_sym(wheel_steer + stick_steer);

    const float stick_throttle = m_level[size_t(Action::StickUp)];
    p.raw_throttle = clamp_unit(stick_throttle);

    const float stick_brake = m_level[size_t(Action::StickDown)];
    p.raw_brake = clamp_unit(stick_brake);

    uint32_t btns = 0;
    for (size_t i = 0; i < size_t(Action::kActionCount); ++i) {
        if (!m_custom_controls && m_level[i] > 0.5f)
            btns |= action_button_bit(static_cast<Action>(i));
    }
    p.custom_steer_analog = clamp_sym(m_level[size_t(Action::DriveAnalogRight)] -
                                      m_level[size_t(Action::DriveAnalogLeft)]);
    p.custom_steer_digital = clamp_sym(m_level[size_t(Action::DriveRight)] -
                                       m_level[size_t(Action::DriveLeft)]);
    p.custom_throttle = clamp_unit(m_level[size_t(Action::DriveThrottle)]);
    p.custom_brake = clamp_unit(m_level[size_t(Action::DriveBrake)]);
    if (m_custom_controls) {
        // Stable transport bits only. The opt-in recomp adapter translates
        // these to the appropriate menu or racing action vocabulary.
        const std::pair<Action, uint32_t> custom_buttons[] = {
            {Action::MenuUp, kBtnDpadUp}, {Action::MenuDown, kBtnDpadDown},
            {Action::MenuLeft, kBtnDpadLeft}, {Action::MenuRight, kBtnDpadRight},
            {Action::MenuConfirm, kBtnA}, {Action::MenuBack, kBtnB},
            {Action::DriveAids, kBtnC}, {Action::DrivePause, kBtnStart},
            {Action::DriveShiftUp, kBtnR}, {Action::DriveShiftDown, kBtnL},
            {Action::DriveView, kBtnD},
            {Action::DriveScreenshot, kBtnF},
        };
        // shooter2d has no translation layer: main tests these raw bits itself
        // (docs/game/shooter2d.md).
        const std::pair<Action, uint32_t> shooter_buttons[] = {
            {Action::ShooterFire, kBtnA}, {Action::ShooterBeam, kBtnB},
            {Action::ShooterStart, kBtnC}, {Action::DrivePause, kBtnStart},
            {Action::DriveScreenshot, kBtnR},
        };
        if (m_shooter_active) {
            for (const auto& [action, bit] : shooter_buttons)
                if (m_level[size_t(action)] > 0.5f) btns |= bit;
        } else {
            for (const auto& [action, bit] : custom_buttons)
                if (m_level[size_t(action)] > 0.5f) btns |= bit;
        }
        const std::pair<Action, uint32_t> directions[] = {
            {Action::DigitalUp, kBtnDpadUp}, {Action::AnalogUp, kBtnDpadUp},
            {Action::DigitalDown, kBtnDpadDown}, {Action::AnalogDown, kBtnDpadDown},
            {Action::DriveLeft, kBtnDpadLeft}, {Action::DriveAnalogLeft, kBtnDpadLeft},
            {Action::DriveRight, kBtnDpadRight}, {Action::DriveAnalogRight, kBtnDpadRight},
        };
        for (const auto& [action, bit] : directions)
            if (m_level[size_t(action)] > 0.5f) btns |= bit;
    }
    p.buttons = btns;

    // Edge masks accumulate across rebuilds until begin_frame().
    p.pressed |= btns & ~m_prev_buttons;
    p.released |= m_prev_buttons & ~btns;
    m_prev_buttons = btns;

    p.device_index = 0;
    p.pack_bytes();

    if (m_level[size_t(Action::Quit)] > 0.5f) m_quit = true;
}

void InputMap::set_live_levels(const Levels& levels) {
    // Host profiles never reinterpret recorded guest input or broker events.
    if (m2host::trace::input_recorder().is_replaying() ||
        m2host::trace::input_recorder().is_broker_replaying()) return;
    m_level = levels;
    tick();
}

void InputMap::tick() {
    rebuild_port();
    // Input recorder: record port 0 with its guest-visible frame sequence.
    // When replaying, live input is suppressed (injected replay is the source).
    // Skip the guest clock query and record construction entirely unless a
    // recording file or the input trace kind is active.
    auto& recorder = m2host::trace::input_recorder();
    if (recorder.is_replaying() || !recorder.wants_capture()) return;

    uint32_t gf = 0;
#ifdef M2_HAVE_RECOMP
    uint32_t seq=0, pc=0, st=0, snap[4]={0};
    if (m2_guest_frame_query(&seq,&pc,&st,snap,4)==0) gf = seq;
#endif
    const auto& p = m_ports[0];
    recorder.record(p.buttons, p.pressed, p.released,
                    p.stick_x, p.stick_y, p.device_index, gf);
}

void InputMap::inject_replay(uint32_t buttons, uint32_t pressed, uint32_t released,
                           uint8_t stick_x, uint8_t stick_y, uint8_t device) {
    ControlPortState& p = m_ports[0];
    p.buttons = buttons;
    p.pressed = pressed;
    p.released = released;
    p.stick_x = stick_x;
    p.stick_y = stick_y;
    p.device_index = device;
    m_prev_buttons = buttons;
}

}  // namespace m2host::input
