// input_selftest.cpp — headless verification: SDL events -> HostControls
// levels -> control-port state -> event-broker protocol, no window/SDL video
// required.
//
// Headless input→controls test, registered with CTest as m2_input_selftest
// (exit 0 = all checks pass).
#include <SDL3/SDL.h>
#include "sdl_adapter.h"
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "folio_loader.h"
#include "shim_abi.h"
#include "broker_bridge.h"
#include "input_map.h"
#include "input_recorder.h"

using namespace m2host;
using namespace m2host::input;

// Registration hooks for subsystems deliberately NOT linked into this test
// (audio_shim/mpeg_decoder pull OggStream+FFmpeg). The stub
// table still exercises every code path this test touches.
void audio_shim_register() {}
void mpeg_decoder_register() {}

namespace {

int g_failures = 0;

// One private scratch directory per invocation. Parallel CTest runs launch
// independent selftest processes, so a fixed name under the system temp root
// would let one process delete another's profiles/replay files.
std::filesystem::path g_scratch_directory;

std::uint64_t process_id() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(_getpid());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

std::filesystem::path make_scratch_directory() {
    const auto parent = std::filesystem::temp_directory_path();
    const auto stamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::error_code last_error;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        char name[128];
        std::snprintf(name, sizeof(name), "m2-input-selftest-%llu-%llx-%u",
                      static_cast<unsigned long long>(process_id()),
                      static_cast<unsigned long long>(stamp), attempt);
        const auto candidate = parent / name;
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error))
            return candidate;
        last_error = error;
    }
    throw std::filesystem::filesystem_error(
        "m2-input-selftest: cannot create a private scratch directory",
        parent, last_error);
}

struct ScratchDirectoryCleanup {
    ~ScratchDirectoryCleanup() {
        if (g_scratch_directory.empty()) return;
        std::error_code error;
        std::filesystem::remove_all(g_scratch_directory, error);
    }
};

void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

void check_near(float got, float want, float eps, const char* what) {
    check(std::fabs(got - want) <= eps, what);
    if (std::fabs(got - want) > eps)
        std::printf("       got %f want %f\n", got, want);
}

using Levels = std::array<float, size_t(Action::kActionCount)>;

Levels level(Action action, float value = 1.f) {
    Levels levels{};
    levels[size_t(action)] = value;
    return levels;
}

// Exercise persisted device mapping at the real SDL polling / replay boundary.
void test_host_profiles() {
    const auto scratch = g_scratch_directory;
    std::filesystem::create_directories(scratch);
    const auto path = scratch / "launcher-controls-test.cfg";
    const auto replay = scratch / "launcher-replay-test.ndjson";
    check(SDL_InitSubSystem(SDL_INIT_JOYSTICK), "SDL joystick subsystem initializes");
    {
        // A run without --controls-profile uses these defaults, so they must
        // cover every exposed pad button, including the vendor extended D/E/F.
        HostControls defaults; defaults.defaults();
        const Action pad_buttons[] = {
            Action::DpadUp, Action::DpadDown, Action::DpadLeft, Action::DpadRight,
            Action::A, Action::B, Action::C, Action::X, Action::L, Action::R, Action::P,
            Action::D, Action::E, Action::F,
        };
        bool every_button_bound = true;
        for (Action action : pad_buttons) {
            bool bound = false;
            for (const auto& control : defaults.bindings)
                bound |= control.action == action && control.binding.kind == HostBinding::Key;
            every_button_bound &= bound;
        }
        check(every_button_bound, "launcher default profile binds every exposed pad button");
        InputMap default_map; SdlInputAdapter default_adapter;
        auto tap = [&](SDL_Scancode scancode) {
            SDL_Event tap_event{}; tap_event.type = SDL_EVENT_KEY_DOWN;
            tap_event.key.scancode = scancode;
            SDL_PushEvent(&tap_event); default_adapter.poll_sdl(default_map, defaults);
            const uint32_t buttons = default_map.control_port(0).buttons;
            tap_event.type = SDL_EVENT_KEY_UP;
            SDL_PushEvent(&tap_event); default_adapter.poll_sdl(default_map, defaults);
            return buttons;
        };
        check(tap(SDL_SCANCODE_B) == kBtnD, "launcher default B reaches the extended D button");
        check(tap(SDL_SCANCODE_N) == kBtnE, "launcher default N reaches the extended E button");
        check(tap(SDL_SCANCODE_M) == kBtnF, "launcher default M reaches the extended F button");
    }
    {
        // The launcher's Bind action replaces the selected action's previous
        // input instead of adding to it.
        HostControls controls; controls.defaults();
        const auto bindings_for = [&](Action action) {
            size_t count = 0;
            for (const auto& control : controls.bindings) count += control.action == action;
            return count;
        };
        HostBinding key; key.id = SDL_SCANCODE_K;
        controls.set_binding(Action::A, key);
        check(bindings_for(Action::A) == 1, "rebinding replaces the previous key");
        bool old_key_gone = true;
        for (const auto& control : controls.bindings)
            if (control.action == Action::A && control.binding.kind == HostBinding::Key)
                old_key_gone &= control.binding.id == SDL_SCANCODE_K;
        check(old_key_gone, "the action's previous key is no longer bound");

        HostBinding pad; pad.kind = HostBinding::JoyButton; pad.device = "pad-guid"; pad.id = 3;
        controls.set_binding(Action::A, pad);
        check(bindings_for(Action::A) == 1, "a controller binding replaces the action's previous binding");
        bool pad_bound = false;
        for (const auto& control : controls.bindings)
            if (control.action == Action::A) pad_bound = control.binding.kind == HostBinding::JoyButton;
        check(pad_bound, "the action is now bound to the controller input");
        pad.id = 4;
        controls.set_binding(Action::A, pad);
        check(bindings_for(Action::A) == 1, "controller rebinding replaces the previous button");
        bool new_pad_bound = false;
        for (const auto& control : controls.bindings)
            if (control.action == Action::A) new_pad_bound = control.binding.id == 4;
        check(new_pad_bound, "the action's previous controller button is no longer bound");

        controls.clear_bindings(Action::A);
        check(bindings_for(Action::A) == 0, "clearing removes the action's binding");

        HostBinding left; left.kind = HostBinding::JoyAxis; left.device = "pad-guid"; left.id = 0;
        left.rest = 0.f; left.endpoint = -1.f;
        HostBinding right = left; right.endpoint = 1.f;
        check(!same_input(left, right), "opposite axis directions are different inputs");
        check(same_input(left, left), "an input matches itself");
        HostBinding key_k; key_k.id = SDL_SCANCODE_K;
        HostBinding key_l; key_l.id = SDL_SCANCODE_L;
        check(!same_input(key_k, key_l) && same_input(key_k, key_k), "keys compare by scancode");
    }
    {
        HostControls controls; controls.defaults();
        controls.bindings.clear();
        HostBinding key; key.id = SDL_SCANCODE_W;
        controls.bindings.push_back({Action::DpadUp, key});
        key.id = SDL_SCANCODE_UP;
        controls.bindings.push_back({Action::DpadUp, key});
        std::string error;
        check(controls.save(path, error), "host profile saves");
        HostControls loaded;
        check(loaded.load(path, error) && loaded.bindings.size() == 2, "host profile reload preserves multiple bindings");
        InputMap map; SdlInputAdapter adapter;
        SDL_Event e{}; e.type = SDL_EVENT_KEY_DOWN; e.key.scancode = SDL_SCANCODE_W;
        SDL_PushEvent(&e); adapter.poll_sdl(map, loaded);
        check(map.control_port(0).buttons == kBtnDpadUp, "saved physical key reaches guest control port");
        e.key.scancode = SDL_SCANCODE_UP; SDL_PushEvent(&e); adapter.poll_sdl(map, loaded);
        e.type = SDL_EVENT_KEY_UP; e.key.scancode = SDL_SCANCODE_W; SDL_PushEvent(&e); adapter.poll_sdl(map, loaded);
        check(map.control_port(0).buttons == kBtnDpadUp, "releasing one binding preserves other held binding");
        e.type = SDL_EVENT_WINDOW_FOCUS_LOST; SDL_PushEvent(&e); adapter.poll_sdl(map, loaded);
        check(map.control_port(0).buttons == 0, "profile releases all controls on focus loss");

        SDL_VirtualJoystickDesc desc{}; SDL_INIT_INTERFACE(&desc);
        desc.name = "Launcher profile validation"; desc.naxes = 3; desc.nbuttons = 1;
        const auto id = SDL_AttachVirtualJoystick(&desc);
        check(id != 0, "SDL virtual joystick attaches");
        loaded.open_devices(); SDL_Joystick* joystick = SDL_OpenJoystick(id);
        if (joystick) {
            SDL_SetJoystickVirtualAxis(joystick, 0, -32768); SDL_UpdateJoysticks();
            loaded.start_capture();
            SDL_Event axis{}; axis.type = SDL_EVENT_JOYSTICK_AXIS_MOTION;
            axis.jaxis.which = id; axis.jaxis.axis = 0; axis.jaxis.value = 32767;
            HostBinding binding;
            check(loaded.capture(axis, binding) && binding.rest == -1 && binding.endpoint == 1,
                  "pedal capture preserves full-range rest and endpoint");
            loaded.bindings = {{Action::DpadUp, binding}}; loaded.reset();
            loaded.event(axis); check(loaded.levels()[size_t(Action::DpadUp)] == 1, "full pedal travel reaches one");
            axis.jaxis.value = -32768; loaded.event(axis);
            check(loaded.levels()[size_t(Action::DpadUp)] == 0, "released full-range pedal reaches zero");
            axis.jaxis.which = id + 100000; axis.jaxis.value = 32767; loaded.event(axis);
            check(loaded.levels()[size_t(Action::DpadUp)] == 0, "unbound device cannot activate pedal");
            check(loaded.save(path, error), "device profile saves stable identity");
            HostControls reloaded; check(reloaded.load(path, error) && reloaded.bindings[0].binding.device == binding.device,
                  "device identity and calibration survive reload");
            HostBinding brake = binding; brake.id = 1;
            HostBinding steering = binding; steering.id = 2; steering.rest = 0.f; steering.deadzone = 0;
            HostBinding right_key; right_key.id = SDL_SCANCODE_RIGHT;
            HostBinding left_key; left_key.id = SDL_SCANCODE_LEFT;
            loaded.bindings = {{Action::DriveThrottle, binding}, {Action::DriveBrake, brake},
                {Action::DriveAnalogRight, steering}, {Action::DriveRight, right_key}, {Action::DriveLeft, left_key}};
            loaded.reset(); map.set_custom_controls(true);
            axis.jaxis.which = id; axis.jaxis.axis = 0; axis.jaxis.value = 32767; loaded.event(axis);
            axis.jaxis.axis = 1; axis.jaxis.value = 16384; loaded.event(axis);
            axis.jaxis.axis = 2; axis.jaxis.value = 16384; loaded.event(axis);
            map.set_live_levels(loaded.levels());
            check(map.control_port(0).custom_throttle == 1.f &&
                      map.control_port(0).custom_brake > .7f && map.control_port(0).custom_brake < .8f,
                  "Custom keeps independent simultaneous analog pedals through the live map");
            check(map.control_port(0).custom_steer_analog > .49f && map.control_port(0).custom_steer_analog < .51f &&
                      map.control_port(0).custom_steer_digital == 0.f &&
                      map.control_port(0).buttons == kBtnDpadRight,
                  "Custom analog steering also reaches the menu direction");
            SDL_Event key_event{}; key_event.type = SDL_EVENT_KEY_DOWN; key_event.key.scancode = SDL_SCANCODE_RIGHT;
            loaded.event(key_event);
            map.set_live_levels(loaded.levels());
            check(map.control_port(0).custom_steer_analog > .49f && map.control_port(0).custom_steer_analog < .51f &&
                      map.control_port(0).custom_steer_digital == 1.f,
                  "digital steering stays separate from the held analog binding");
            key_event.type = SDL_EVENT_KEY_UP; loaded.event(key_event);
            map.set_live_levels(loaded.levels());
            check(map.control_port(0).custom_steer_analog > .49f && map.control_port(0).custom_steer_analog < .51f &&
                      map.control_port(0).custom_steer_digital == 0.f,
                  "releasing D-pad steering preserves held analog steering");
            key_event.type = SDL_EVENT_KEY_DOWN; key_event.key.scancode = SDL_SCANCODE_LEFT;
            loaded.event(key_event);
            map.set_live_levels(loaded.levels());
            check(map.control_port(0).custom_steer_analog > .49f && map.control_port(0).custom_steer_analog < .51f &&
                      map.control_port(0).custom_steer_digital == -1.f,
                  "opposing digital and analog steering keep their own signs");
            check(loaded.save(path, error) && reloaded.load(path, error) && reloaded.bindings.size() == 5,
                  "Custom pedals and multiple steering bindings survive profile reload");
            key_event.type = SDL_EVENT_WINDOW_FOCUS_LOST; loaded.event(key_event);
            map.set_live_levels(loaded.levels());
            check(map.control_port(0).custom_steer_analog == 0 && map.control_port(0).custom_steer_digital == 0 &&
                      map.control_port(0).custom_throttle == 0 && map.control_port(0).custom_brake == 0,
                  "focus loss releases all Custom steering and pedal channels");
            loaded.bindings = {{Action::DriveRight, steering}, {Action::DriveAnalogLeft, left_key}};
            loaded.reset();
            axis.jaxis.axis = 2; axis.jaxis.value = 32767; loaded.event(axis);
            key_event.type = SDL_EVENT_KEY_DOWN; key_event.key.scancode = SDL_SCANCODE_LEFT; loaded.event(key_event);
            map.set_live_levels(loaded.levels());
            check(map.control_port(0).custom_steer_digital == 1.f && map.control_port(0).custom_steer_analog == -1.f,
                  "steering channels follow the bound action, not the input kind");
            map.set_custom_controls(false);
            SDL_CloseJoystick(joystick);
        }
        SDL_DetachVirtualJoystick(id);

        // Button and hat bindings follow the same path as the launcher's
        // capture: recorded identity, live event, action level, guest port.
        SDL_VirtualJoystickDesc pad_desc{};
        SDL_INIT_INTERFACE(&pad_desc);
        pad_desc.name = "Launcher button validation";
        pad_desc.nbuttons = 2;
        pad_desc.nhats = 1;
        const auto pad_id = SDL_AttachVirtualJoystick(&pad_desc);
        check(pad_id != 0, "SDL virtual button pad attaches");
        SDL_UpdateJoysticks();
        loaded.open_devices();
        if (SDL_Joystick* pad = SDL_OpenJoystick(pad_id)) {
            loaded.start_capture();
            SDL_Event button{}; button.type = SDL_EVENT_JOYSTICK_BUTTON_DOWN;
            button.jbutton.which = pad_id; button.jbutton.button = 1;
            button.jbutton.down = true;
            HostBinding button_binding;
            check(loaded.capture(button, button_binding) &&
                      button_binding.kind == HostBinding::JoyButton &&
                      !button_binding.device.empty(),
                  "controller button capture records device and button");
            loaded.bindings = {{Action::A, button_binding}};
            loaded.reset();
            loaded.event(button);
            check(loaded.levels()[size_t(Action::A)] == 1, "controller button press reaches action level");
            InputMap button_map;
            SdlInputAdapter button_adapter;
            SDL_PushEvent(&button);
            button_adapter.poll_sdl(button_map, loaded);
            check(button_map.control_port(0).buttons == kBtnA, "controller button reaches guest control port");
            SDL_Event release = button;
            release.type = SDL_EVENT_JOYSTICK_BUTTON_UP;
            release.jbutton.down = false;
            loaded.event(release);
            check(loaded.levels()[size_t(Action::A)] == 0, "controller button release clears action level");

            loaded.start_capture();
            SDL_Event hat{}; hat.type = SDL_EVENT_JOYSTICK_HAT_MOTION;
            hat.jhat.which = pad_id; hat.jhat.hat = 0; hat.jhat.value = SDL_HAT_UP;
            HostBinding hat_binding;
            check(loaded.capture(hat, hat_binding) &&
                      hat_binding.kind == HostBinding::JoyHat &&
                      hat_binding.id == 1,
                  "controller hat capture records direction");
            loaded.bindings = {{Action::DpadUp, hat_binding}};
            loaded.reset();
            loaded.event(hat);
            check(loaded.levels()[size_t(Action::DpadUp)] == 1, "controller hat press reaches action level");
            SDL_CloseJoystick(pad);
        }
        SDL_DetachVirtualJoystick(pad_id);
        {
            // Idle polls happen on every SDL event and frame; they must not
            // retain records or touch the trace. A recording run still must.
            auto& recorder = m2host::trace::input_recorder();
            recorder.stop_recording();
            recorder.stop_replay();
            recorder.clear_buffer();
            InputMap idle_map;
            for (int i = 0; i < 64; ++i) idle_map.tick();
            check(recorder.recorded_count() == 0,
                  "idle map polls retain no input records");
            const auto tick_path = scratch / "tick-recording.ndjson";
            check(recorder.start_recording(tick_path.string()),
                  "tick recording opens");
            idle_map.tick();
            recorder.stop_recording();
            check(recorder.recorded_count() == 1,
                  "active recording captures a map poll");
            recorder.clear_buffer();
        }
        auto& recorder = m2host::trace::input_recorder();
        check(recorder.start_recording(replay.string()), "existing replay writer opens");
        recorder.record(kBtnA, kBtnA, kBtnB, 23, 219, 3, 17);
        recorder.stop_recording();
        check(recorder.start_replay(replay.string()), "existing replay reader loads unchanged format");
        m2host::trace::InputRecord record;
        check(recorder.pop_for_frame(17, &record), "existing replay record retains frame scheduling");
        map.inject_replay(record.buttons, record.pressed, record.released, record.stick_x, record.stick_y, record.device);
        auto levels = loaded.levels(); levels.fill(1.f); map.set_live_levels(levels);
        e = {}; e.type = SDL_EVENT_KEY_DOWN; e.key.scancode = SDL_SCANCODE_W;
        SDL_PushEvent(&e); adapter.poll_sdl(map, loaded, true);
        const auto& cp = map.control_port(0);
        check(cp.buttons == kBtnA && cp.pressed == kBtnA && cp.released == kBtnB && cp.stick_x == 23 && cp.stick_y == 219 && cp.device_index == 3,
              "saved bindings and live SDL input leave every replay control field unchanged");
        e.type = SDL_EVENT_QUIT; SDL_PushEvent(&e); adapter.poll_sdl(map, loaded, true);
        check(adapter.close_requested(), "window close still works during replay");
        recorder.stop_replay();
        // A manual movie run can hold the same guest frame while distinct
        // input levels arrive hundreds of milliseconds apart. Scheduling all
        // three at that frame would erase the held button before consumption.
        {
            std::ofstream timed(replay);
            timed << "{\"sequence\":0,\"host_time\":50,\"guest_frame\":17,\"buttons\":0,\"pressed\":0,\"released\":0,\"stick_x\":128,\"stick_y\":128,\"device\":0}\n"
                  << "{\"sequence\":1,\"host_time\":150,\"guest_frame\":17,\"buttons\":1,\"pressed\":1,\"released\":0,\"stick_x\":128,\"stick_y\":128,\"device\":0}\n"
                  << "{\"sequence\":2,\"host_time\":250,\"guest_frame\":17,\"buttons\":0,\"pressed\":0,\"released\":1,\"stick_x\":128,\"stick_y\":128,\"device\":0}\n";
        }
        check(recorder.start_replay(replay.string()), "timestamp replay loads manual input levels");
        check(recorder.pop_for_host_time(0, &record) && record.buttons == 0,
              "timestamp replay normalizes the first recorded time");
        check(!recorder.pop_for_host_time(99, &record), "timestamp replay holds a future press");
        check(recorder.pop_for_host_time(100, &record) && record.buttons == kBtnA,
              "timestamp replay delivers the press at its recorded interval");
        check(!recorder.pop_for_host_time(199, &record), "timestamp replay preserves the held interval at a parked frame");
        check(recorder.pop_for_host_time(200, &record) && record.released == kBtnA,
              "timestamp replay releases without needing another guest frame");
        check(!recorder.pop_for_host_time(1000, &record), "timestamp replay stops at end of recording");
        recorder.stop_replay();
        check(recorder.start_replay(replay.string()), "default frame replay still loads timestamp records");
        check(recorder.pop_for_frame(17, &record) && recorder.pop_for_frame(17, &record) &&
              recorder.pop_for_frame(17, &record) && record.released == kBtnA,
              "default frame scheduling remains unchanged");
        recorder.stop_replay();
    }
    {
        HostControls cleared; cleared.defaults();
        for (size_t i = size_t(Action::DriveLeft); i < size_t(Action::kActionCount); ++i)
            cleared.clear_bindings(Action(i));
        const size_t hardware = cleared.bindings.size();
        std::string error;
        HostControls reloaded;
        check(cleared.save(path, error) && reloaded.load(path, error) &&
                  reloaded.bindings.size() == hardware,
              "a deliberately cleared Custom profile stays cleared after reload");
    }
    {
        InputMap map;
        map.set_custom_controls(true);
        InputMap::Levels levels{};
        levels[size_t(Action::DriveAnalogLeft)] = 0.75f;
        levels[size_t(Action::DigitalUp)] = 1.f;
        map.set_live_levels(levels);
        check((map.control_port(0).buttons & (kBtnDpadLeft | kBtnDpadUp)) == (kBtnDpadLeft | kBtnDpadUp) &&
                  map.control_port(0).custom_steer_analog < -0.7f,
              "Custom directions reach both menu buttons and proportional steering");
        map.set_custom_controls(false);
        map.set_live_levels(levels);
        check((map.control_port(0).buttons & (kBtnDpadLeft | kBtnDpadUp)) == 0,
              "Custom directions do not emit menu buttons in other modes");
    }
    {
        Action unknown;
        check(!action_from_name("drive_clutch", &unknown),
              "there is no manual clutch action");
        std::ofstream other(path);
        other << "m2-host-controls 5\n" << "drive_screenshot 0 \"\" 43 1 0 1 0.08\n";
        other.close();
        HostControls rejected; std::string error;
        check(!rejected.load(path, error), "profiles with another format version are rejected");
        std::ofstream legacy(path);
        legacy << "m2-host-controls 1\n"
               << "drive_left 6 \"pad\" 0 1 0 -1 0.08\n" << "drive_right 6 \"pad\" 0 1 0 1 0.08\n"
               << "drive_left 0 \"\" 80 1 0 1 0.08\n";
        legacy.close();
        HostControls migrated;
        check(migrated.load(path, error) && migrated.bindings.size() == 3 &&
                  migrated.bindings[0].action == Action::DriveAnalogLeft &&
                  migrated.bindings[1].action == Action::DriveAnalogRight &&
                  migrated.bindings[2].action == Action::DriveLeft,
              "version 1 axis steering moves to the analog steering actions");
        HostControls resaved;
        check(migrated.save(path, error) && resaved.load(path, error) &&
                  resaved.bindings[0].action == Action::DriveAnalogLeft,
              "migrated steering saves in the current format");
        std::ofstream old(path);
        old << "m2-host-controls 2\n"
            << "drive_left 0 \"\" 80 1 0 1 0.08\n"
            << "menu_left 0 \"\" 80 1 0 1 0.08\n"
            << "menu_up 6 \"pad\" 1 1 0 -1 0.08\n"
            << "menu_down 0 \"\" 81 1 0 1 0.08\n";
        old.close();
        HostControls combined;
        check(combined.load(path, error) && combined.bindings.size() == 3 &&
                  combined.bindings[0].action == Action::DriveLeft &&
                  combined.bindings[1].action == Action::AnalogUp &&
                  combined.bindings[2].action == Action::DigitalDown,
              "legacy menu directions migrate and duplicate steering bindings merge");
        std::ofstream pre_shooter(path);
        pre_shooter << "m2-host-controls 3\n"
                    << "menu_confirm 4 \"pad\" 0 1 0 1 0\n"
                    << "menu_back 4 \"pad\" 1 1 0 1 0\n"
                    << "drive_aids 4 \"pad\" 2 1 0 1 0\n"
                    << "drive_pause 4 \"pad\" 3 1 0 1 0\n";
        pre_shooter.close();
        HostControls upgraded;
        const auto bound_to = [&](Action action, int id) {
            return std::any_of(upgraded.bindings.begin(), upgraded.bindings.end(),
                               [&](const HostControl& c) {
                                   return c.action == action && c.binding.id == id && c.binding.device == "pad";
                               });
        };
        check(upgraded.load(path, error) && upgraded.bindings.size() == 7 &&
                  bound_to(Action::MenuConfirm, 0) && bound_to(Action::ShooterFire, 0) &&
                  bound_to(Action::ShooterBeam, 1) && bound_to(Action::ShooterStart, 2),
              "older profiles copy their shooter2d bindings from the racing actions");
        upgraded.clear_bindings(Action::ShooterFire);
        HostControls cleared_shooter;
        check(upgraded.save(path, error) && cleared_shooter.load(path, error) &&
                  cleared_shooter.bindings.size() == 6,
              "a cleared shooter2d binding is not copied back after reload");
    }
    {
        HostBinding key; key.id = SDL_SCANCODE_F9;
        HostControls configured, loaded; std::string error;
        configured.bindings = {{Action::DriveScreenshot, key}};
        check(configured.save(path, error) && loaded.load(path, error) &&
                  loaded.bindings.size() == 1 && loaded.bindings[0].action == Action::DriveScreenshot,
              "Custom screenshot binding survives profile reload");
        InputMap map; map.set_custom_controls(true);
        SDL_Event event{}; event.type = SDL_EVENT_KEY_DOWN; event.key.scancode = SDL_SCANCODE_F9;
        loaded.event(event); map.set_live_levels(loaded.levels());
        check(map.control_port(0).buttons == kBtnF, "Custom screenshot reaches its private transport bit");
        event.type = SDL_EVENT_KEY_UP; loaded.event(event); map.set_live_levels(loaded.levels());
        check(map.control_port(0).buttons == 0, "Custom screenshot releases after the press");
    }
    std::filesystem::remove(path); std::filesystem::remove(replay);
    SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
}

// --- SDL delivery through the default profile -------------------------------
// Exercise the actual SDL queue, including releases arriving between polls.
void test_sdl_delivery() {
    check(SDL_InitSubSystem(SDL_INIT_EVENTS), "SDL events initialize");
    InputMap map;
    HostControls controls; controls.defaults();
    SdlInputAdapter adapter;
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.scancode = SDL_SCANCODE_DOWN;
    SDL_PushEvent(&event);
    adapter.poll_sdl(map, controls);
    check(map.control_port(0).buttons == kBtnDpadDown &&
              map.control_port(0).stick_y == 128,
          "SDL Down reaches D-pad without analog pedal travel");
    event.type = SDL_EVENT_KEY_UP;
    SDL_PushEvent(&event);
    adapter.poll_sdl(map, controls);
    check(map.control_port(0).buttons == 0 && map.control_port(0).stick_y == 128,
          "SDL release snaps direction to neutral");
    map.begin_frame();
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.scancode = SDL_SCANCODE_X;
    SDL_PushEvent(&event);
    event.type = SDL_EVENT_KEY_UP;
    SDL_PushEvent(&event);
    adapter.poll_sdl(map, controls);
    check(map.control_port(0).buttons == 0 &&
              (map.control_port(0).pressed & kBtnA) != 0 &&
              (map.control_port(0).released & kBtnA) != 0,
          "a press and release within one poll both leave edges");
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.scancode = SDL_SCANCODE_RIGHT;
    SDL_PushEvent(&event);
    adapter.poll_sdl(map, controls);
    event = {};
    event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    SDL_PushEvent(&event);
    adapter.poll_sdl(map, controls);
    check(map.control_port(0).buttons == 0 && map.control_port(0).stick_x == 128,
          "focus loss releases held direction");
    SDL_QuitSubSystem(SDL_INIT_EVENTS);
}

// --- axis packing ----------------------------------------------------------------
void test_packing() {
    std::printf("axis packing\n");
    // byte packing mirrors guest clamp path
    check(axis_to_byte(0.f) == 128.f, "axis_to_byte center = 128");
    check(axis_to_byte(1.f) == 255.f, "axis_to_byte full = 255");
    check(axis_to_byte(-1.f) == 1.f, "axis_to_byte full negative = 1");
}

// --- analog path through InputMap ---------------------------------------------------
void test_analog() {
    std::printf("analog controls\n");
    InputMap im;
    Levels levels{};
    levels[size_t(Action::StickRight)] = 0.5f;
    levels[size_t(Action::StickUp)] = 1.f;
    levels[size_t(Action::StickDown)] = 0.4f;
    im.set_live_levels(levels);
    const ControlPortState& p = im.control_port(0);
    check_near(p.raw_steer, 0.5f, 1e-6f, "stick right travel reaches raw steer");
    check_near(p.raw_throttle, 1.f, 1e-6f, "stick up travel reaches raw throttle");
    check_near(p.raw_brake, 0.4f, 1e-6f, "stick down travel reaches raw brake");
    check(p.stick_x == uint8_t(axis_to_byte(p.raw_steer) + 0.5f),
          "stick_x packed from raw steer");
    check(p.stick_y == uint8_t(axis_to_byte(p.raw_brake - p.raw_throttle) + 0.5f),
          "stick_y packed from raw pedals (brake positive)");
    InputMap throttle_only;
    throttle_only.set_live_levels(level(Action::StickUp));
    check(throttle_only.control_port(0).stick_y == 1,
          "throttle packs to the negative side of stick_y");
    InputMap brake_only;
    brake_only.set_live_levels(level(Action::StickDown));
    check(brake_only.control_port(0).stick_y == 255,
          "brake packs to the positive side of stick_y");
    check(p.buttons == 0, "analog travel does not synthesize buttons");
    // The driver layer publishes the raw device position (vendor StickDriver.c
    // does no deadzone/gamma shaping), so even a small deflection reaches the
    // guest and its own FilterStick/FilterControl stage decides what to do.
    InputMap small;
    small.set_live_levels(level(Action::StickRight, 0.05f));
    check(small.control_port(0).stick_x != 128 &&
              small.control_port(0).stick_x ==
                  uint8_t(axis_to_byte(small.control_port(0).raw_steer) + 0.5f),
          "small deflection published raw, not host-zeroed");
    InputMap wheel;
    Levels opposed{};
    opposed[size_t(Action::WheelLeft)] = 1.f;
    opposed[size_t(Action::StickRight)] = 1.f;
    wheel.set_live_levels(opposed);
    check(wheel.control_port(0).stick_x == 128, "opposing wheel and stick travel cancel");
}

// --- buttons & edges --------------------------------------------------------------
void test_edges() {
    std::printf("button edges across frames\n");
    const struct { Action action; uint32_t bit; } buttons[] = {
        {Action::DpadUp, kBtnDpadUp}, {Action::DpadDown, kBtnDpadDown},
        {Action::DpadLeft, kBtnDpadLeft}, {Action::DpadRight, kBtnDpadRight},
        {Action::A, kBtnA}, {Action::B, kBtnB}, {Action::C, kBtnC},
        {Action::X, kBtnX}, {Action::L, kBtnL}, {Action::R, kBtnR},
        {Action::P, kBtnStart}, {Action::D, kBtnD}, {Action::E, kBtnE},
        {Action::F, kBtnF},
    };
    for (const auto& button : buttons) {
        InputMap im;
        im.begin_frame();
        im.set_live_levels(level(button.action));
        const ControlPortState& p = im.control_port(0);
        check(p.buttons == button.bit && p.pressed == button.bit &&
                  p.stick_x == 128 && p.stick_y == 128,
              "pad action publishes only its digital button");
        im.set_live_levels({});
        check(p.buttons == 0 && p.released == button.bit,
              "pad button release edge latched the same frame");
        im.begin_frame();
        check(p.pressed == 0 && p.released == 0, "begin_frame clears edges");
        Action parsed;
        check(action_from_name(action_name(button.action), &parsed) &&
                  parsed == button.action, "profile token round trip");
    }
    InputMap half;
    half.set_live_levels(level(Action::A, 0.5f));
    check(half.control_port(0).buttons == 0, "a level of one half does not press a button");

    InputMap custom;
    custom.set_custom_controls(true);
    custom.set_live_levels(level(Action::A));
    check(custom.control_port(0).buttons == 0, "Custom mode ignores hardware button actions");
    custom.set_live_levels(level(Action::MenuConfirm));
    check(custom.control_port(0).buttons == kBtnA, "Custom confirm uses the A transport bit");

    // Racing and shooter2d read different Custom actions for the same pad bits.
    const std::pair<Action, uint32_t> racing_buttons[] = {
        {Action::ShooterFire, 0u}, {Action::ShooterBeam, 0u}, {Action::ShooterStart, 0u},
        {Action::DriveShiftUp, kBtnR}, {Action::DriveScreenshot, kBtnF},
    };
    for (const auto& [action, bit] : racing_buttons) {
        custom.set_live_levels(level(action));
        check(custom.control_port(0).buttons == bit, "racing keeps its Custom button mapping");
    }
    custom.set_shooter_active(true);
    const std::pair<Action, uint32_t> shooter_buttons[] = {
        {Action::ShooterFire, kBtnA}, {Action::ShooterBeam, kBtnB}, {Action::ShooterStart, kBtnC},
        {Action::DrivePause, kBtnStart}, {Action::DriveScreenshot, kBtnR},
        {Action::DigitalUp, kBtnDpadUp}, {Action::DriveAnalogLeft, kBtnDpadLeft},
        {Action::MenuConfirm, 0u}, {Action::MenuBack, 0u}, {Action::DriveAids, 0u},
        {Action::DriveShiftUp, 0u}, {Action::DriveShiftDown, 0u},
        {Action::DriveView, 0u}, {Action::DriveThrottle, 0u},
    };
    for (const auto& [action, bit] : shooter_buttons) {
        custom.set_live_levels(level(action));
        check(custom.control_port(0).buttons == bit, "shooter2d uses its own Custom button mapping");
    }
    InputMap shooter_original;
    shooter_original.set_shooter_active(true);
    shooter_original.set_live_levels(level(Action::R));
    check(shooter_original.control_port(0).buttons == kBtnR,
          "the shooter mapping applies only to Custom");

    InputMap quit;
    check(!quit.quit_requested(), "no quit before the Quit action");
    quit.set_live_levels(level(Action::Quit));
    check(quit.quit_requested() && quit.control_port(0).buttons == 0,
          "Quit action requests quit without a pad button");
    quit.set_live_levels({});
    check(quit.quit_requested(), "quit request latches after release");
}

// --- kernel msgport protocol (ConnectEventBroker sequence shape) ------------------------
typedef m2_item_t (*fn_find_named)(int32_t, const char*);
typedef m2_item_t (*fn_create_msgport)(const char*, int32_t, int32_t);
typedef m2_item_t (*fn_create_bufmsg)(int32_t, int32_t, m2_item_t, uint32_t);
typedef int32_t (*fn_send_msg)(m2_item_t, m2_item_t, void*, uint32_t);
typedef m2_item_t (*fn_wait_port)(m2_item_t, m2_item_t);
typedef void* (*fn_lookup_item)(int32_t, const char*);

template <typename T>
static T sel(uint32_t folio, uint32_t ord) {
    return reinterpret_cast<T>(m2_selector_lookup(folio, ord));
}

void test_broker_protocol() {
    std::printf("kernel msgport protocol (ConnectEventBroker call shape)\n");
    folio_install_all();     // fills stub table (kernel/iff/... live)
    input_shim_register();   // overrides (176/172/168/150/130/34)
    check(broker_bridge_ready(), "bridge registered");

    // Point replies at a live snapshot so SendMsg mirrors input.
    InputMap im;
    im.set_live_levels(level(Action::DpadRight));
    broker_bridge_set_control_port(&im.control_port(0));

    // Exactly the FN_ConnectEventBroker__0f770 selector sequence:
    auto find_named = sel<fn_find_named>(M2_FOLIO_KERNEL, 176);
    auto create_port = sel<fn_create_msgport>(M2_FOLIO_KERNEL, 172);
    auto create_msg = sel<fn_create_bufmsg>(M2_FOLIO_KERNEL, 168);
    auto send = sel<fn_send_msg>(M2_FOLIO_KERNEL, 150);
    auto wait = sel<fn_wait_port>(M2_FOLIO_KERNEL, 130);
    auto lookup = sel<fn_lookup_item>(M2_FOLIO_KERNEL, 34);

    const m2_item_t broker = find_named(0x10a, "eventbroker");
    check(broker != 0 && broker == broker_bridge_broker_item(),
          "FindNamedItem(\"eventbroker\") resolves");
    check(find_named(0x10a, "nosuchthing") == 0, "unknown name not found");

    const m2_item_t port = create_port("listener", 0, 0);
    check(port != 0, "CreateMsgPort returns item");
    const m2_item_t msg = create_msg(0, 0, port, 0x100);
    check(msg != 0, "CreateBufferedMsg returns item");

    // registration message {1, 2} size 0x6c (per lift @0xf82c..0xf870)
    uint32_t body[2] = {1, 2};
    check(send(broker, msg, body, 0x6c) == M2_OK, "SendMsg(registration) ok");
    const m2_item_t done = wait(port, msg);
    check(done == msg, "WaitPort completes immediately (single-thread host)");

    // Reply record: LookupItem derefs to host memory with +0x28 count/+0x2c list
    uint8_t* rec = static_cast<uint8_t*>(lookup(msg, nullptr));
    check(rec != nullptr, "LookupItem resolves bridge record");
    if (rec) {
        uint32_t count = 0;
        std::memcpy(&count, rec + 0x28, 4);
        check(count == 1, "reply device count == 1");
        uintptr_t node = 0;
        std::memcpy(&node, rec + 0x2c, sizeof(node));
        check(node != 0, "reply device-list pointer non-null");
        if (node) {
            const char* desc = *reinterpret_cast<const char**>(node + 4);
            check(desc && std::strstr(desc, "EventBroker") != nullptr,
                  "device descriptor printable");
        }

        // Second round-trip: query message {0x1d} size 4 (ConnectEventBroker
        // second SendMsg), then verify the live-input mirror in the tail.
    }
    uint32_t q = 0x1d;
    check(send(broker, msg, &q, 4) == M2_OK, "SendMsg(query) ok");
    check(wait(port, msg) == msg, "WaitPort(query) ok");
    rec = static_cast<uint8_t*>(lookup(msg, nullptr));
    check(rec != nullptr, "record stable across second round-trip");
    if (rec) {
        // Live-input mirror at +0x38..+0x3e (PROVISIONAL protocol tail)
        uint32_t btns = 0;
        std::memcpy(&btns, rec + 0x38, 4);
        check(btns == kBtnDpadRight,
              "right D-pad bit mirrors steering direction");
        check(rec[0x3c] == im.control_port(0).stick_x,
              "stick_x mirrored into reply tail");
        check(rec[0x3e] == 0, "device index mirrored");
    }

    broker_bridge_set_control_port(nullptr);
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);  // survive crashes with full log
    std::printf("=== input selftest ===\n");
    g_scratch_directory = make_scratch_directory();
    ScratchDirectoryCleanup scratch_cleanup;
    test_sdl_delivery();
    test_host_profiles();
    test_packing();
    test_analog();
    test_edges();
    test_broker_protocol();
    std::error_code ec;
    std::filesystem::remove_all(g_scratch_directory, ec);
    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
