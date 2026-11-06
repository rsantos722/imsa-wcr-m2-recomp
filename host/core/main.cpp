// main.cpp — native host entry for the recompiled guest.
//
// The host supplies modern platform services and the frame/input boundary;
// guest code owns application state and behavior.
//
// Usage:
//   m2host [--headless] [--root DIR] [--game-data DIR] [--width N] [--height N] [--frames N]
//          [--fast-forward] [--capture DIR] [--perf-file PATH]
//          [discovery/savestate options]
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include "guest_display.h"
#include "platform.h"
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <signal.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static std::atomic<bool> s_quit_requested{false};
static void host_sigint_handler(int)
{
    s_quit_requested.store(true, std::memory_order_relaxed);
}

#ifdef __APPLE__
#include <execinfo.h>
#include <unistd.h>
#endif

extern "C" {
#include "shim_abi.h"
}
// m2_kernel_dispatch registry access for the post-entry folio report probe.
// Local rename: recomp_helpers.h (C, guest side) defines struct m2_tag_arg
// {uint32_t value}; shims/shim_types.h later in this TU redefines that same
// class name as {int32_t value}, which C++ rejects. The guest TagArg type is
// unused here, so hide it under its own tag name for this include only.
#define m2_tag_arg m2_guest_tag_arg
#include "recomp_helpers.h"
#include "diagnostic_input.h"
#undef m2_tag_arg
#include "audio_shim.h"
#include "audio/ogg_stream.h"
#include "audio_ogg.h"
#include "config.h"
#include "control_modes.h"
#include "data_paths.h"
#include "event_broker.h"
#include "filesystem_shim.h"
#include "folio_loader.h"
#include "selector_probe.h"
#include "graphics/cel_blitter.h"
#include "graphics/guest_clt.h"
#include "graphics/letterbox.h"
#include "graphics/soft_capture.h"
#include "input/broker_bridge.h"
#include "input/host_controls.h"
#include "input/input_map.h"
#include "input/sdl_adapter.h"
#include "mpeg_decoder.h"
#include "diagnostic_perf.h"
#include "savestate.h"
#include "diagnostic_control.h"
#include "trace.h"
#include "input_recorder.h"
#include "vbl_pacer.h"
#include "ppc_ctx.h"

#if defined(_WIN32)
// Ask NVIDIA Optimus and AMD switchable graphics drivers for the dedicated GPU.
// The drivers look for these in the executable's export table, so they live
// here and not in a library.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

extern "C" int m2_game_entry(void);
extern "C" void m2_recomp_shutdown(void);
extern "C" int m2_guest_live_task_count(void);
extern "C" void m2_guest_kernel_lock_diag_dump(void);
extern "C" void m2_kernel_lock_diag_dump_pairs(void);
extern "C" void m2_audio_graph_lock_diag_dump(void);
extern "C" struct PPCContext* m2_recomp_context(void);
extern "C" void m2_recomp_set_game_root(const char* root);
extern "C" const char* m2_recomp_get_game_root(void);
#ifdef M2_HAVE_RECOMP
extern "C" int m2_guest_prepare_restore(void);
#endif
extern "C" uint64_t m2_oob_trap_total(void);
// Shared state-driven input driver (runtime/include/adaptive_driver.h).
#include "adaptive_driver.h"

namespace {

// Presentation pacing (M2_VBL_PACED_SWAP): present with non-blocking swap
// (swap interval 0) and pace the interactive loop on the rational NTSC VBL
// deadline instead, so a missed display swap cannot stretch the guest's field.
// Enabled by default; M2_VBL_PACED_SWAP=0 or --no-vbl-paced-swap disables it.
inline bool vbl_paced_swap_requested() {
    const char* e = std::getenv("M2_VBL_PACED_SWAP");
    if (e && *e) return *e != '0';
    return true;
}

struct Config {
    bool headless = false;
    bool fast_forward = false;
    // --fullscreen: exclusive fullscreen at the desktop mode.
    // --borderless-fullscreen: desktop-sized borderless window. Either way the
    // 4:3 image is fitted inside the display with black bars.
    bool fullscreen = false;
    bool borderless_fullscreen = false;
    bool vbl_paced_swap = vbl_paced_swap_requested();
    int width = 640;
    int height = 480;
    int frames = 0;  // 0 = run until quit; >0 = fixed count
    std::filesystem::path root;
    std::filesystem::path game_data_dir;  // --game-data: folder containing driving/game
    std::filesystem::path capture_dir;  // --capture: dump headless PPM frames
    std::filesystem::path screenshot_path; // --screenshot: capture single PPM frame
    int screenshot_frame = -1;          // --screenshot-frame: specific host frame (-1 = final frame)
    // Trace and input recorder
    std::string trace_file;
    std::string perf_file;
    std::string trace_filter;
    uint32_t trace_frames = 0;
    bool trace_no_enter_exit = false;
    bool trace_stderr = false;
    std::string record_input;
    std::string controls_profile;
    bool analog_controller = false;  // --controller-type joystick
    m2_controls_mode controls_mode = M2_CONTROLS_ORIGINAL;
    bool improved_analog_steering = false;
    std::string replay_input;
    bool replay_input_host_time = false;
    std::string record_broker;
    std::string replay_broker;
    std::string savestate_load;
    std::string savestate_save;
    std::string control_socket;
    std::string music_dir;
    // Developer guest-memory writes: --poke EA=VAL, applied after game entry.
    std::vector<std::pair<uint32_t,uint32_t>> pokes;
};

// Host settings files live in the same per-user folder as the launcher's
// settings (SDL pref path), never in the working tree. Falls back to the
// working directory only when the platform has no pref path.
std::filesystem::path host_config_path(const char* name) {
    const std::filesystem::path dir = m2host::input::launcher_settings_directory();
    return dir.empty() ? std::filesystem::path(name) : dir / name;
}

// The folio selector probes live in selector_probe.cpp so tests can link them
// without the SDL host; see selector_probe.h.

// --- state-driven input (adaptive driver) ----------------------------------
// Reproduces State 2 with two diagnostic sequences
// instead of pinned frame numbers: M2_PREVIEW_MODE=hold waits for primary
// 2 / secondary 25 and holds; M2_PREVIEW_MODE=advance presses/releases A
// after the state-25 settle and continues only after the guest publishes
// secondary 26 or primary 3.  The guest decides every transition; the host
// only supplies the stimulus.  Off by default (no input override).

constexpr uint32_t kBtnA = 1u << 0;
constexpr uint32_t kBtnDpadDown = 1u << 9;
constexpr uint32_t kBtnDpadRight = 1u << 11;

unsigned char preview_read8(void* user, uint32_t ea) {
    (void)user;
    PPCContext* ctx = m2_recomp_context();
    if (!ctx || !ctx->mem || ea >= RECOMP_GUEST_MEM) return 0;
    return rd8(ctx, ea);
}

struct PreviewDriver {
    m2_driver drv{};
    bool active = false;
    bool advance = false;
    bool has_tail = false;
    uint32_t tail_buttons = 0;
    uint8_t tail_sx = 128;
    uint8_t tail_sy = 128;
};

PreviewDriver& preview_driver() {
    static PreviewDriver pd;
    return pd;
}

// Returns true when M2_PREVIEW_MODE selects a driver sequence. Only game-enabled
// builds call it.
[[maybe_unused]] bool preview_driver_init() {
    const char* mode = std::getenv("M2_PREVIEW_MODE");
    if (!mode || mode[0] == '\0') return false;
    PreviewDriver& pd = preview_driver();
    pd.advance = (std::strcmp(mode, "advance") == 0 ||
                  std::strcmp(mode, "advance_drive") == 0 ||
                  std::strcmp(mode, "1") == 0);
    if (std::strcmp(mode, "advance_drive") == 0) {
        pd.has_tail = true;
        pd.tail_sy = 255;  // full throttle down start straight
    }
    if (const char* eb = std::getenv("M2_TAIL_BUTTONS")) {
        char* end = nullptr;
        unsigned long v = std::strtoul(eb, &end, 0);
        if (end != eb) {
            pd.tail_buttons = static_cast<uint32_t>(v);
            pd.has_tail = true;
        }
    }
    if (const char* ex = std::getenv("M2_TAIL_STICK_X")) {
        char* end = nullptr;
        unsigned long v = std::strtoul(ex, &end, 0);
        if (end != ex) {
            pd.tail_sx = static_cast<uint8_t>(v & 0xffu);
            pd.has_tail = true;
        }
    }
    if (const char* ey = std::getenv("M2_TAIL_STICK_Y")) {
        char* end = nullptr;
        unsigned long v = std::strtoul(ey, &end, 0);
        if (end != ey) {
            pd.tail_sy = static_cast<uint8_t>(v & 0xffu);
            pd.has_tail = true;
        }
    }
    static const m2_drv_step hold_steps[] = {
        {.kind = M2_DRV_WAIT_STATE, .a = 1},
        {.kind = M2_DRV_STEP_UNTIL, .a = kBtnDpadDown, .b = 0x17d94b, .c = 0,
         .d = 6, .e = 20, .sx = 128, .sy = 255},
        {.kind = M2_DRV_PRESS, .a = kBtnA, .b = 10, .c = 20},
        {.kind = M2_DRV_WAIT_SEC, .a = 25},
        {.kind = M2_DRV_WAIT_FRAMES, .a = 40},
    };
    // Advance mode: proven race navigation (mirrors probe_s3_clt.c) —
    // menu cursor to option 0, enter Car Select (sec 25), select and confirm
    // car, wait for Track Select (sec 26), confirm track, reach State 3.
    static const m2_drv_step advance_steps[] = {
        {.kind = M2_DRV_WAIT_STATE, .a = 1},
        {.kind = M2_DRV_STEP_UNTIL, .a = kBtnDpadDown, .b = 0x17d94b, .c = 0,
         .d = 6, .e = 20, .sx = 128, .sy = 255},
        {.kind = M2_DRV_PRESS, .a = kBtnA, .b = 10, .c = 20},
        {.kind = M2_DRV_WAIT_SEC, .a = 25},
        {.kind = M2_DRV_WAIT_FRAMES, .a = 40},
        {.kind = M2_DRV_PRESS, .a = kBtnA, .b = 6, .c = 20},  /* select car       */
        {.kind = M2_DRV_PRESS, .a = kBtnA, .b = 6, .c = 20},  /* confirm car      */
        {.kind = M2_DRV_WAIT_SEC, .a = 26},
        {.kind = M2_DRV_WAIT_FRAMES, .a = 30},
        {.kind = M2_DRV_PRESS, .a = kBtnA, .b = 6, .c = 20},  /* confirm track    */
        {.kind = M2_DRV_WAIT_STATE, .a = 3},
    };
    // Bounded track selection for the advance route: M2_PREVIEW_TRACK_PULSES
    // D-pad Right pulses on the sec-26 track screen before the confirm press
    // (0 = the scripted default track). The 4-held / 12-released pulse shape
    // mirrors the documented state-2 probe's paced stepping, which keeps a
    // single press from stepping twice through the menu's hold-repeat gate;
    // the guest confirms the track
    // itself, so a missed pulse stops the route instead of loading another
    // track silently.
    const m2_drv_step* steps = advance_steps;
    int step_count =
        static_cast<int>(sizeof(advance_steps) / sizeof(advance_steps[0]));
    uint32_t track_pulses = 0;
    if (const char* ep = std::getenv("M2_PREVIEW_TRACK_PULSES")) {
        char* end = nullptr;
        const unsigned long v = std::strtoul(ep, &end, 0);
        if (end != ep && *end == '\0' && v <= 8ul) {
            track_pulses = static_cast<uint32_t>(v);
        }
    }
    static m2_drv_step advance_track_steps[
        sizeof(advance_steps) / sizeof(advance_steps[0]) + 10];
    if (pd.advance && track_pulses > 0u) {
        // Everything through the sec-26 settle (index 8), the pulses and a
        // short settle, then the script's confirm press and State-3 wait.
        int n = 0;
        for (; n < 9; ++n) advance_track_steps[n] = advance_steps[n];
        for (uint32_t pulse = 0; pulse < track_pulses; ++pulse) {
            m2_drv_step step = {};
            step.kind = M2_DRV_PRESS;
            step.a = kBtnDpadRight;
            step.b = 4;   // held guest frames per pulse
            step.c = 12;  // released guest frames (repeat-gate reset)
            step.sx = 255;
            step.sy = 128;
            advance_track_steps[n++] = step;
        }
        m2_drv_step settle = {};
        settle.kind = M2_DRV_WAIT_FRAMES;
        settle.a = 6;
        advance_track_steps[n++] = settle;
        for (int i = 9; i < step_count; ++i)
            advance_track_steps[n++] = advance_steps[i];
        steps = advance_track_steps;
        step_count = n;
    }
    if (pd.advance) {
        m2_drv_init(&pd.drv, steps, step_count, preview_read8, nullptr);
    } else {
        m2_drv_init(&pd.drv, hold_steps,
                    static_cast<int>(sizeof(hold_steps) /
                                     sizeof(hold_steps[0])),
                    preview_read8, nullptr);
    }
    pd.active = true;
    std::printf("[preview-drv] mode=%s active (tail: %s, buttons=%#x sx=%u sy=%u)\n",
                mode, pd.has_tail ? "yes" : "no", pd.tail_buttons, pd.tail_sx, pd.tail_sy);
    return true;
}

bool save_ppm_screenshot(m2host::ICelBlitterBackend* gfx, const std::filesystem::path& path)
{
    if (!gfx || path.empty()) {
        std::fprintf(stderr, "[screenshot] no backend or output path\n");
        return false;
    }
    const std::string path_utf8 = m2host::platform::path_to_utf8(path);
    int w = 0, h = 0;
    gfx->get_size(&w, &h);
    if (w <= 0 || h <= 0) {
        std::fprintf(stderr, "[screenshot] invalid frame size %dx%d\n", w, h);
        return false;
    }
    std::vector<uint32_t> rgba(static_cast<size_t>(w) * h);
    if (!gfx->grab_presented_rgba(rgba.data(), w, h)) {
        std::fprintf(stderr, "[screenshot] grab failed (backend=%s)\n",
                     gfx->name());
        return false;
    }
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            std::fprintf(stderr, "[screenshot] create directory failed for %s: %s\n",
                         path_utf8.c_str(), ec.message().c_str());
            return false;
        }
    }
    errno = 0;
    FILE* f = m2host::platform::fopen(path, "wb");
    if (!f) {
        const int err = errno;
        std::fprintf(stderr, "[screenshot] open failed for %s: %s\n",
                     path_utf8.c_str(),
                     err ? std::strerror(err) : "unknown error");
        return false;
    }
    bool ok = true;
    int write_errno = 0;
    if (std::fprintf(f, "P6\n%d %d\n255\n", w, h) < 0) {
        ok = false;
        write_errno = errno;
    }
    // Write one complete row at a time so short/failed writes are observable
    // instead of being discarded per pixel.
    std::vector<uint8_t> row(static_cast<size_t>(w) * 3u);
    for (int y = 0; ok && y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint32_t px = rgba[static_cast<size_t>(y) * w + x];
            row[static_cast<size_t>(x) * 3u + 0u] =
                static_cast<uint8_t>(px & 0xffu);
            row[static_cast<size_t>(x) * 3u + 1u] =
                static_cast<uint8_t>((px >> 8u) & 0xffu);
            row[static_cast<size_t>(x) * 3u + 2u] =
                static_cast<uint8_t>((px >> 16u) & 0xffu);
        }
        if (std::fwrite(row.data(), 1, row.size(), f) != row.size()) {
            ok = false;
            write_errno = errno;
        }
    }
    // fflush catches a deferred stdio error; fclose catches a failure while
    // flushing the final buffer and is always called.
    if (ok && std::fflush(f) != 0) {
        ok = false;
        write_errno = errno;
    }
    if (std::fclose(f) != 0 && ok) {
        ok = false;
        write_errno = errno;
    }
    if (!ok) {
        std::fprintf(stderr, "[screenshot] write failed for %s: %s\n",
                     path_utf8.c_str(),
                     write_errno ? std::strerror(write_errno)
                                 : "I/O error");
        return false;
    }
    std::printf("[screenshot] %dx%d frame saved to %s\n", w, h,
                path_utf8.c_str());
    std::fflush(stdout);
    return true;
}

// --- opt-in interactive shots ----------------------------------------------
// M2_SHOT_DIR: write PPM snapshots of the just-rendered frame for an
// interactive (windowed) run so GL-backend content can be verified
// programmatically; the existing headless --capture path already writes
// PPMs via the software backend.
// M2_SHOT_FRAMES: comma-separated 0-based host frame numbers to snap.

struct ShotSink {
    bool active = false;
    std::vector<int> frames;
    std::filesystem::path dir;
    std::size_t idx = 0;

    static ShotSink& instance()
    {
        static ShotSink s;
        return s;
    }

    void init()
    {
        const char* d = std::getenv("M2_SHOT_DIR");
        if (!d || !d[0]) return;
        dir = m2host::platform::path_from_utf8(d);
        active = true;
        if (const char* f = std::getenv("M2_SHOT_FRAMES")) {
            std::stringstream ss(f);
            std::string part;
            while (std::getline(ss, part, ',')) {
                part.erase(0, part.find_first_not_of(" \t"));
                if (!part.empty()) frames.push_back(std::atoi(part.c_str()));
            }
        }
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            std::fprintf(stderr, "[shots] create directory failed for %s: %s\n",
                         m2host::platform::path_to_utf8(dir).c_str(),
                         ec.message().c_str());
            active = false;
            return;
        }
        std::printf("[shots] M2_SHOT_DIR=%s frames=%zu\n",
                    m2host::platform::path_to_utf8(dir).c_str(), frames.size());
    }

    void maybe_shot(m2host::ICelBlitterBackend* gfx, int host_frame)
    {
        if (!active || idx >= frames.size()) return;
        if (frames[idx] != host_frame) return;
        ++idx;
        char name[64];
        std::snprintf(name, sizeof(name), "shot_%05d.ppm", host_frame);
        const std::filesystem::path path = dir / name;
        // Interactive diagnostic capture: save_ppm_screenshot diagnoses a
        // failed output, but it must not end an otherwise healthy session.
        if (!save_ppm_screenshot(gfx, path)) {
            std::fprintf(stderr, "[shots] frame %d capture failed\n",
                         host_frame);
            return;
        }
        std::printf("[shots] frame %d -> %s\n", host_frame,
                    m2host::platform::path_to_utf8(path).c_str());
    }
};

// --- opt-in screenshot hotkey ----------------------------------------------
// M2_SCREENSHOT_KEY: in a windowed run, F12 saves the presented frame as a PNG
// at the backend's internal render size (M2_RENDER_SCALE applies), into
// M2_SCREENSHOT_ROOT or the default screenshots folder. Host-only: the guest
// never sees the key.
struct HotkeyShot {
    bool active = false;
    bool was_down = false;
    std::filesystem::path dir;

    static HotkeyShot& instance()
    {
        static HotkeyShot s;
        return s;
    }

    void init()
    {
        const char* v = std::getenv("M2_SCREENSHOT_KEY");
        if (!v || !v[0] || std::strcmp(v, "0") == 0) return;
        dir = m2host::platform::get_env_path("M2_SCREENSHOT_ROOT");
        if (dir.empty()) dir = m2host::platform::default_screenshot_dir();
        active = true;
        std::printf("[hotkey-shot] F12 saves PNG screenshots to %s\n",
                    m2host::platform::path_to_utf8(dir).c_str());
    }

    void poll(m2host::ICelBlitterBackend* gfx, int host_frame)
    {
        if (!active) return;
        const bool* keys = SDL_GetKeyboardState(nullptr);
        const bool down = keys && keys[SDL_SCANCODE_F12];
        const bool pressed = down && !was_down;
        was_down = down;
        if (pressed) save(gfx, host_frame);
    }

private:
    void save(m2host::ICelBlitterBackend* gfx, int host_frame)
    {
        int w = 0, h = 0;
        if (gfx) gfx->get_size(&w, &h);
        if (w <= 0 || h <= 0) {
            std::fprintf(stderr, "[hotkey-shot] no frame to capture\n");
            return;
        }
        std::vector<uint32_t> rgba(static_cast<size_t>(w) * h);
        if (!gfx->grab_presented_rgba(rgba.data(), w, h)) {
            std::fprintf(stderr, "[hotkey-shot] grab failed (backend=%s)\n",
                         gfx->name());
            return;
        }
        // Targets may carry metadata in alpha; a screenshot is opaque.
        for (uint32_t& px : rgba) px |= 0xff000000u;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            std::fprintf(stderr, "[hotkey-shot] create directory failed for %s: %s\n",
                         m2host::platform::path_to_utf8(dir).c_str(),
                         ec.message().c_str());
            return;
        }
        const std::time_t now = std::time(nullptr);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
        char name[80];
        std::snprintf(name, sizeof(name), "m2-%s-f%d.png", stamp, host_frame);
        const std::string path = m2host::platform::path_to_utf8(dir / name);
        SDL_Surface* surface = SDL_CreateSurfaceFrom(
            w, h, SDL_PIXELFORMAT_RGBA32, rgba.data(), w * 4);
        const bool ok = surface && SDL_SavePNG(surface, path.c_str());
        if (surface) SDL_DestroySurface(surface);
        if (!ok) {
            std::fprintf(stderr, "[hotkey-shot] save failed for %s: %s\n",
                         path.c_str(), SDL_GetError());
            return;
        }
        std::printf("[hotkey-shot] %dx%d frame %d -> %s\n", w, h, host_frame,
                    path.c_str());
        std::fflush(stdout);
    }
};

// --- opt-in display-carve content dump --------------------------------------
// M2_CLT_CARVE_DUMP samples the content of the target a consumed
// DISPLAYFRAMEBUFFER selected, at the moment that display unit is about to be
// presented, and reports how it differs from both the same address's previous
// consumed content and the sibling address's. That is the oracle the
// transition flip is missing: a destination whose content never moves across
// a whole composition sequence is one the guest's composition never reached
// (dropped), while one that moves -- especially one that only moves toward the
// sibling's image it is supposed to replace -- is a composition blended over
// content the guest did not author (stale). A dropped composition shows up as
// `own_px=0`, a stale blend as a changed region whose bounding box is not the
// authored draw extent. M2_CLT_CARVE_DUMP_FROM lower-bounds the host frame;
// the stream and the retained per-address copies are both capped.
// Observation only: the software backend's grab is a plain copy and the GL
// readback restores its read binding, so no render state is left changed.
struct CarveDumpSink {
    bool active = false;
    uint32_t from = 0;
    uint32_t logged = 0;
    uint32_t peer_base = 0;
    uint32_t images_written = 0;
    uint32_t image_max = 64;
    std::filesystem::path image_dir;
    std::map<uint32_t, std::vector<uint32_t>> prev;

    static CarveDumpSink& instance()
    {
        static CarveDumpSink s;
        return s;
    }

    void init()
    {
        const char* v = std::getenv("M2_CLT_CARVE_DUMP");
        if (!v || !v[0] || v[0] == '0') return;
        active = true;
        if (const char* f = std::getenv("M2_CLT_CARVE_DUMP_FROM"))
            from = static_cast<uint32_t>(std::strtoul(f, nullptr, 0));
        // M2_CLT_CARVE_DUMP_DIR additionally writes each sampled carve out as
        // a PPM, so the retained and composed regions of a destination can be
        // inspected directly instead of inferred from a signature.
        if (const char* d = std::getenv("M2_CLT_CARVE_DUMP_DIR")) {
            if (d[0]) {
                image_dir = d;
                if (const char* m = std::getenv("M2_CLT_CARVE_DUMP_IMAGES_MAX"))
                    image_max = static_cast<uint32_t>(
                        std::strtoul(m, nullptr, 0));
                std::error_code ec;
                std::filesystem::create_directories(image_dir, ec);
            }
        }
        std::printf("[carve-dump] enabled from host frame %u images=%s\n", from,
                    image_dir.empty() ? "off" : image_dir.string().c_str());
    }

    void write_image(int host_frame, uint32_t target,
                     const std::vector<uint32_t>& px, int w, int h)
    {
        if (image_dir.empty() || images_written >= image_max) return;
        char name[96];
        std::snprintf(name, sizeof(name), "carve_%05d_%06x.ppm", host_frame,
                      target);
        FILE* f = std::fopen((image_dir / name).string().c_str(), "wb");
        if (!f) return;
        ++images_written;
        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const uint32_t p = px[static_cast<size_t>(y) * w + x];
                const uint8_t rgb[3] = {static_cast<uint8_t>(p & 0xffu),
                                        static_cast<uint8_t>((p >> 8u) & 0xffu),
                                        static_cast<uint8_t>((p >> 16u) & 0xffu)};
                std::fwrite(rgb, 1, 3, f);
            }
        std::fclose(f);
    }

    struct Sig {
        uint64_t hash = 1469598103934665603ull;
        uint64_t r = 0, g = 0, b = 0;
        uint32_t nonclear = 0;
        // Share of pixels holding the single most common 5:5:5 colour, and
        // that colour. A picture the guest composed has many colours; a plane
        // the host filled on first selection, or one holding a depth surface
        // rather than an image, is nearly one colour. This is what separates
        // "the composition never reached this destination" from "the
        // composition was blended over content the guest did not author":
        // neither advances the plane, but only the host-filled one is flat.
        uint32_t modal_pct = 0;
        uint32_t modal = 0;
    };

    static Sig sign(const std::vector<uint32_t>& px)
    {
        Sig s;
        std::vector<uint32_t> hist(32768, 0);
        for (const uint32_t p : px) {
            s.hash ^= p;
            s.hash *= 1099511628211ull;
            s.r += p & 0xffu;
            s.g += (p >> 8u) & 0xffu;
            s.b += (p >> 16u) & 0xffu;
            // Host clear colour 0.05,0.07,0.10 through f2u8 is 13,18,26.
            if ((p & 0x00ffffffu) != 0x001a120du) ++s.nonclear;
            ++hist[((p & 0x1fu) << 10) | (((p >> 8u) & 0x1fu) << 5) |
                   ((p >> 16u) & 0x1fu)];
        }
        for (uint32_t bin = 0; bin < hist.size(); ++bin) {
            if (hist[bin] > s.modal_pct) {
                s.modal_pct = hist[bin];
                s.modal = bin;
            }
        }
        s.modal_pct = px.empty()
                          ? 0
                          : static_cast<uint32_t>(
                                static_cast<uint64_t>(s.modal_pct) * 100u /
                                px.size());
        return s;
    }

    // Difference of two same-size planes: pixel count, changed-pixel bounding
    // box, and the mean absolute per-channel difference. The count alone is
    // not enough to read a pair of images: two destinations that hold the same
    // picture through different rounding differ in most pixels while the
    // magnitude stays at one LSB, which is not the flicker. Only a large
    // magnitude means the two hold genuinely different pictures. An empty box
    // (x1 < x0) means the planes are identical.
    struct Diff {
        uint64_t pixels = 0;
        uint64_t abs_sum = 0;
        int x0 = INT_MAX, y0 = INT_MAX, x1 = -1, y1 = -1;
        // magnitude in hundredths of an 8-bit step, over all pixels
        unsigned mad_x100(size_t total) const
        {
            if (total == 0) return 0;
            return static_cast<unsigned>(
                (abs_sum / total) * 100u / 3u);
        }
    };

    static Diff diff(const std::vector<uint32_t>& a,
                     const std::vector<uint32_t>& b, int w)
    {
        Diff d;
        const size_t n = std::min(a.size(), b.size());
        for (size_t i = 0; i < n; ++i) {
            const uint32_t x0 = a[i], y0 = b[i];
            if (x0 == y0) continue;
            ++d.pixels;
            for (int s = 0; s < 24; s += 8)
                d.abs_sum += static_cast<uint64_t>(
                    std::abs(static_cast<int>((x0 >> s) & 0xffu) -
                             static_cast<int>((y0 >> s) & 0xffu)));
            const int x = static_cast<int>(i % static_cast<size_t>(w));
            const int y = static_cast<int>(i / static_cast<size_t>(w));
            d.x0 = std::min(d.x0, x);
            d.y0 = std::min(d.y0, y);
            d.x1 = std::max(d.x1, x);
            d.y1 = std::max(d.y1, y);
        }
        return d;
    }

    void maybe_dump(m2host::ICelBlitterBackend* gfx, int host_frame,
                    uint32_t target, uint32_t tag, uint32_t consumed,
                    const char* phase)
    {
        if (!active || target == 0 || gfx == nullptr) return;
        if (static_cast<uint32_t>(host_frame) < from) return;
        int w = 0, h = 0;
        gfx->get_size(&w, &h);
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
        const size_t total = static_cast<size_t>(w) * static_cast<size_t>(h);
        std::vector<uint32_t> px(total);
        const bool got = gfx->grab_clt_target_rgba(target, px.data(), w, h);
        if (++logged > 4096u) return;
        char own[64] = "-";
        char peer[96] = "-";
        if (got) {
            const Sig s = sign(px);
            const auto self = prev.find(target);
            if (self != prev.end()) {
                const Diff d = diff(px, self->second, w);
                if (d.x1 < d.x0)
                    std::snprintf(own, sizeof(own),
                                  "own_px=0/%zu own_mad=0", total);
                else
                    std::snprintf(own, sizeof(own),
                                  "own_px=%llu/%zu own_mad=%u "
                                  "own_box=%d,%d-%d,%d",
                                  static_cast<unsigned long long>(d.pixels),
                                  total, d.mad_x100(total), d.x0, d.y0, d.x1,
                                  d.y1);
            }
            if (peer_base != 0 && peer_base != target) {
                const auto other = prev.find(peer_base);
                if (other != prev.end()) {
                    const Diff d = diff(px, other->second, w);
                    if (d.x1 < d.x0)
                        std::snprintf(peer, sizeof(peer),
                                      "peer=%#x peer_px=0/%zu peer_mad=0",
                                      peer_base, total);
                    else
                        std::snprintf(peer, sizeof(peer),
                                      "peer=%#x peer_px=%llu/%zu "
                                      "peer_mad=%u peer_box=%d,%d-%d,%d",
                                      peer_base,
                                      static_cast<unsigned long long>(d.pixels),
                                      total, d.mad_x100(total), d.x0, d.y0,
                                      d.x1, d.y1);
                }
            }
            write_image(host_frame, target, px, w, h);
            if (prev.size() >= 4u) prev.clear();
            prev[target] = px;
            peer_base = target;
            std::fprintf(
                stderr,
                "[carve-dump] host=%d target=%#x tag=%u consumed=%#x "
                "phase=%s got=1 res=%dx%d mean=%llu,%llu,%llu nonclear=%u%% "
                "modal=%u%%/%#x hash=%016llx %s %s\n",
                host_frame, target, tag, consumed, phase ? phase : "-", w, h,
                static_cast<unsigned long long>(s.r / total),
                static_cast<unsigned long long>(s.g / total),
                static_cast<unsigned long long>(s.b / total),
                static_cast<unsigned>(
                    static_cast<uint64_t>(s.nonclear) * 100u / total),
                s.modal_pct, s.modal, static_cast<unsigned long long>(s.hash),
                own, peer);
        } else {
            std::fprintf(stderr,
                         "[carve-dump] host=%d target=%#x tag=%u consumed=%#x "
                         "phase=%s got=0 res=%dx%d %s\n",
                         host_frame, target, tag, consumed,
                         phase ? phase : "-", w, h, own);
        }
        std::fflush(stderr);
    }
};

}  // namespace
int main(int argc, char** argv) {
#ifdef __APPLE__
    // Temporary crash-site capture for the State 2 graphics gate.  It is
    // opt-in so ordinary host runs retain their normal signal behavior.
    if (std::getenv("M2_NATIVE_CRASH_TRACE") != nullptr) {
        auto crash_trace = [](int sig) {
            void* frames[64];
            const int count = backtrace(frames, 64);
            const char prefix[] = "[m2host] fatal signal backtrace\n";
            (void)write(STDERR_FILENO, prefix, sizeof(prefix) - 1u);
            backtrace_symbols_fd(frames, count, STDERR_FILENO);
            _exit(128 + sig);
        };
        signal(SIGBUS, crash_trace);
        signal(SIGSEGV, crash_trace);
        signal(SIGILL, crash_trace);
    }
#endif
    const char* boot_app_env = std::getenv("M2_BOOT_APP");
    const bool boot_shooter2d = boot_app_env &&
        std::strcmp(boot_app_env, "shooter2d") == 0;
    if (boot_app_env && boot_app_env[0] != '\0' &&
        std::strcmp(boot_app_env, "game") != 0 &&
        std::strcmp(boot_app_env, "driving") != 0 &&
        !boot_shooter2d) {
        std::fprintf(stderr,
                     "[main] unsupported M2_BOOT_APP=%s (expected game or shooter2d)\n",
                     boot_app_env);
        return 2;
    }
    m2host::platform::enable_fine_timer_resolution();
    Config cfg;
    const char* improved_steering = std::getenv("M2_IMPROVED_ANALOG_STEERING");
    cfg.improved_analog_steering = improved_steering && std::strcmp(improved_steering, "1") == 0;
    const std::vector<std::string> args = m2host::platform::utf8_args(argc, argv);
    for (int i = 1; i < argc; ++i) {
        const std::string a = args[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? args[++i].c_str() : ""; };
        if (a == "--headless") cfg.headless = true;
        else if (a == "--has-guest") {
            // Capability query for the launcher: exit 0 when this host links
            // the recompiled game, 3 when it carries the stub guest. Answers
            // before any platform, graphics or audio initialization.
#ifdef M2_HAVE_RECOMP
            std::printf("guest: recomp\n");
            return 0;
#else
            std::printf("guest: stub\n");
            return 3;
#endif
        }
        else if (a == "--root") cfg.root = m2host::platform::path_from_utf8(next());
        else if (a == "--game-data") cfg.game_data_dir = m2host::platform::path_from_utf8(next());
        else if (a == "--width") {
            const int v = std::atoi(next());
            if (v < 1 || v > 8192) {
                std::fprintf(stderr, "[main] --width must be 1..8192\n");
                return 2;
            }
            cfg.width = v;
        }
        else if (a == "--height") {
            const int v = std::atoi(next());
            if (v < 1 || v > 8192) {
                std::fprintf(stderr, "[main] --height must be 1..8192\n");
                return 2;
            }
            cfg.height = v;
        }
        else if (a == "--frames") cfg.frames = std::atoi(next());
        else if (a == "--fast-forward") cfg.fast_forward = true;
        else if (a == "--fullscreen") cfg.fullscreen = true;
        else if (a == "--borderless-fullscreen") cfg.borderless_fullscreen = true;
        else if (a == "--capture") cfg.capture_dir = m2host::platform::path_from_utf8(next());
        else if (a == "--screenshot") cfg.screenshot_path = m2host::platform::path_from_utf8(next());
        else if (a == "--screenshot-frame") {
            cfg.screenshot_frame = std::atoi(next());
            if (cfg.screenshot_path.empty()) cfg.screenshot_path = "screenshot.ppm";
        }
        else if (a == "--trace-file") cfg.trace_file = next();
        else if (a == "--perf-file") cfg.perf_file = next();
        else if (a == "--trace-frames") cfg.trace_frames = static_cast<uint32_t>(std::atoi(next()));
        else if (a == "--trace-filter") cfg.trace_filter = next();
        else if (a == "--trace-no-enter-exit") cfg.trace_no_enter_exit = true;
        else if (a == "--trace-stderr") cfg.trace_stderr = true;
        else if (a == "--record-input") cfg.record_input = next();
        else if (a == "--controls-profile") cfg.controls_profile = next();
        else if (a == "--controls-mode") {
            const std::string mode = next();
            if (mode == "original") cfg.controls_mode = M2_CONTROLS_ORIGINAL;
            else if (mode == "patched") cfg.controls_mode = M2_CONTROLS_PATCHED;
            else if (mode == "custom") cfg.controls_mode = M2_CONTROLS_CUSTOM;
            else {
                std::fprintf(stderr, "[main] --controls-mode must be original, patched or custom\n");
                return 2;
            }
        }
        else if (a == "--controller-type") {
            const std::string type = next();
            if (type == "pad") cfg.analog_controller = false;
            else if (type == "joystick") cfg.analog_controller = true;
            else {
                std::fprintf(stderr, "[main] --controller-type must be pad or joystick\n");
                return 2;
            }
        }
        else if (a == "--replay-input") cfg.replay_input = next();
        else if (a == "--replay-input-host-time") cfg.replay_input_host_time = true;
        else if (a == "--record-broker") cfg.record_broker = next();
        else if (a == "--replay-broker") cfg.replay_broker = next();
        else if (a == "--savestate-load") cfg.savestate_load = next();
        else if (a == "--savestate-save") cfg.savestate_save = next();
        else if (a == "--music-dir") cfg.music_dir = next();
        else if (a == "--control-socket") cfg.control_socket = next();
        else if (a == "--vbl-paced-swap") cfg.vbl_paced_swap = true;
        else if (a == "--no-vbl-paced-swap") cfg.vbl_paced_swap = false;
        else if (a == "--poke") {
            std::string spec = next();
            // format: <ea>=<val>  hex or dec, ea is guest EA, val is u32
            size_t eq = spec.find('=');
            if (eq==std::string::npos) { std::fprintf(stderr, "--poke requires ea=val\n"); return 2; }
            std::string ea_s = spec.substr(0,eq), val_s = spec.substr(eq+1);
            try {
                uint32_t ea = static_cast<uint32_t>(std::stoul(ea_s,nullptr,0));
                uint32_t val = static_cast<uint32_t>(std::stoul(val_s,nullptr,0));
                cfg.pokes.emplace_back(ea,val);
            } catch(...) { std::fprintf(stderr, "--poke parse failed: %s\n", spec.c_str()); return 2; }
        }
        else if (a == "--poke:help" || a == "--help" || a == "-h") {
            std::printf("Usage: m2host [--headless] [--root DIR] [--game-data DIR] [--frames N] [--width N] [--height N] [--fast-forward] [--fullscreen|--borderless-fullscreen] [--vbl-paced-swap|--no-vbl-paced-swap] [--capture DIR] [--screenshot PATH] [--screenshot-frame N] [--trace-file PATH] [--perf-file PATH] [--trace-frames N] [--trace-filter kinds] [--trace-no-enter-exit] [--trace-stderr] [--controls-profile PATH] [--controls-mode original|patched|custom] [--controller-type pad|joystick] [--record-input PATH] [--replay-input PATH] [--record-broker PATH] [--replay-broker PATH] [--savestate-load PATH] [--savestate-save PATH] [--music-dir DIR] [--poke EA=VAL]\n");
            std::printf("  --root DIR and --game-data DIR: --root is the guest filesystem root; --game-data is the folder containing driving/game. If --game-data is omitted, it is inferred from --root.\n");
            std::printf("  --width N / --height N: host window and capture size in pixels (1..8192; default 640x480, 4:3).\n");
            std::printf("  --has-guest: print whether this host links the recompiled game and exit (0 = game, 3 = stub guest).\n");
            std::printf("  The preservation path runs the recovered guest; no host gameplay fallback exists.\n");
            std::printf("  --fullscreen / --borderless-fullscreen: exclusive or borderless desktop fullscreen; the 4:3 image keeps its aspect with black bars.\n");
            std::printf("  --vbl-paced-swap / --no-vbl-paced-swap: pace presentation on rational NTSC VBL deadline with swap interval 0 (default: enabled).\n");
            std::printf("  --control-socket PATH: paused diagnostic session; use tools/m2ctl.py for input, waits, memory and captures.\n");
            std::printf("  --replay-input-host-time: schedule recorded control-port inputs by their original elapsed milliseconds; requires windowed real-time playback.\n");
            std::printf("  M2_DISPLAY_INDEX=N: place the windowed presentation on SDL display N (0-based); unset keeps the default.\n");
            std::printf("  --music-dir DIR: index converted .ogg music under DIR/{audio,sfx} (optional; off by default).\n");
            std::printf("  --poke EA=VAL: developer tool; write a 32-bit guest word after game entry (repeatable, hex or decimal).\n");
            std::printf("  --trace-file PATH: NDJSON trace with schema_version/source_image_hash/build_id/run_id/sequence/host_time/guest_frame/task_id/guest_pc/event_kind/payload\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown arg %s\n", a.c_str());
            return 2;
        }
    }
    if (!cfg.control_socket.empty()) {
        if (!cfg.replay_input.empty() || !cfg.replay_broker.empty() ||
            !cfg.savestate_load.empty() || !cfg.savestate_save.empty() ||
            !cfg.pokes.empty() || std::getenv("M2_PREVIEW_MODE")) {
            std::fprintf(stderr, "diagnostic control requires exclusive input and a fresh guest boot\n");
            return 2;
        }
    }
    if (cfg.replay_input_host_time && (cfg.replay_input.empty() || cfg.headless ||
        cfg.fast_forward || !cfg.control_socket.empty() || !cfg.replay_broker.empty())) {
        std::fprintf(stderr, "--replay-input-host-time requires --replay-input with windowed real-time playback and no diagnostic control or broker replay\n");
        return 2;
    }
    // Input and broker recordings hold hardware samples only, with no mode or
    // independent pedal channels, so they record and replay in Original mode.
    if (!cfg.replay_input.empty() || !cfg.replay_broker.empty() || !cfg.control_socket.empty()) {
        cfg.controls_mode = M2_CONTROLS_ORIGINAL;
        cfg.improved_analog_steering = false;
    }
    if (cfg.improved_analog_steering &&
        (!cfg.record_input.empty() || !cfg.record_broker.empty())) {
        std::fprintf(stderr, "[controls] Input/broker recording requires Improved Analog Steering off\n");
        return 2;
    }
    if (cfg.controls_mode != M2_CONTROLS_ORIGINAL &&
        (!cfg.record_input.empty() || !cfg.record_broker.empty())) {
        std::fprintf(stderr, "[controls] Input/broker recording requires Original controls mode\n");
        return 2;
    }
    if (cfg.headless && cfg.frames == 0 && cfg.control_socket.empty()) cfg.frames = 4;

    // Resolve the game image root separately from the guest filesystem root.
    // --game-data wins; otherwise accept the extracted root, driving/data, or
    // shooter2d and walk up to the folder containing driving/game.
    if (!cfg.game_data_dir.empty()) {
        const std::filesystem::path normalized =
            m2host::find_game_data_root(cfg.game_data_dir);
        if (!normalized.empty()) {
            cfg.game_data_dir = normalized;
        }
    }
    if (cfg.game_data_dir.empty() && !cfg.root.empty()) {
        cfg.game_data_dir = m2host::find_game_data_root(cfg.root);
    }
    if (cfg.game_data_dir.empty()) {
        const std::filesystem::path env_imsa =
            m2host::platform::get_env_path("IMSA_DATA_DIR");
        const std::filesystem::path env_game = env_imsa.empty()
            ? m2host::platform::get_env_path("GAME_ROOT")
            : std::filesystem::path{};
        const std::filesystem::path candidate =
            !env_imsa.empty() ? env_imsa : env_game;
        if (!candidate.empty()) {
            const std::filesystem::path normalized =
                m2host::find_game_data_root(candidate);
            cfg.game_data_dir = normalized.empty() ? candidate : normalized;
        }
    }
#ifdef M2_HAVE_RECOMP
    if (cfg.game_data_dir.empty()) {
        const char* recomp_root = m2_recomp_get_game_root();
        if (recomp_root && *recomp_root) {
            const std::filesystem::path p =
                m2host::platform::path_from_utf8(recomp_root);
            const std::filesystem::path normalized =
                m2host::find_game_data_root(p);
            cfg.game_data_dir = normalized.empty() ? p : normalized;
        }
    }
#endif
#ifdef IMSA_DEFAULT_DATA_DIR
    if (cfg.game_data_dir.empty()) {
        const std::filesystem::path p =
            m2host::platform::path_from_utf8(IMSA_DEFAULT_DATA_DIR);
        if (!p.empty()) {
            const std::filesystem::path normalized =
                m2host::find_game_data_root(p);
            cfg.game_data_dir = normalized.empty() ? p : normalized;
        }
    }
#endif
    if (cfg.game_data_dir.empty() && !cfg.root.empty()) {
        std::fprintf(stderr,
                     "[main] warning: could not infer game data from --root %s; "
                     "use --game-data\n",
                     m2host::platform::path_to_utf8(cfg.root).c_str());
    }
    if (!cfg.game_data_dir.empty()) {
        cfg.root = m2host::resolve_guest_root(cfg.game_data_dir, cfg.root, boot_shooter2d);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(
                cfg.game_data_dir / "driving" / "game", ec)) {
            std::fprintf(stderr,
                         "[main] warning: %s does not contain driving/game\n",
                         m2host::platform::path_to_utf8(cfg.game_data_dir).c_str());
        }
        const std::string game_data_utf8 =
            m2host::platform::path_to_utf8(cfg.game_data_dir);
#ifdef M2_HAVE_RECOMP
        m2_recomp_set_game_root(game_data_utf8.c_str());
#endif
        std::printf("[main] game data root: %s\n", game_data_utf8.c_str());
    }
    if (m2host::perf::init(cfg.perf_file)) {
        if (cfg.perf_file.empty())
            std::printf("[perf] enabled\n");
        else
            std::printf("[perf] enabled — report=%s\n", cfg.perf_file.c_str());
    }

    // --- trace and perf init -----------------------------------------------------------
    {
        m2host::trace::TraceConfig tcfg;
        tcfg.file_path = cfg.trace_file;
        tcfg.max_frames = cfg.trace_frames;
        tcfg.enable_enter_exit = !cfg.trace_no_enter_exit;
        tcfg.mirror_stderr = cfg.trace_stderr;
        if (!cfg.trace_filter.empty()) {
            std::stringstream ss(cfg.trace_filter);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                // trim
                size_t b = tok.find_first_not_of(" \t\r\n");
                size_t e = tok.find_last_not_of(" \t\r\n");
                if (b==std::string::npos) continue;
                tok = tok.substr(b, e-b+1);
                m2host::trace::EventKind k;
                if (m2host::trace::event_kind_from_name(tok, &k)) tcfg.filter.push_back(k);
                else std::fprintf(stderr, "[trace] unknown filter kind %s\n", tok.c_str());
            }
        }
        // Auto-enable trace file for --record-input / --replay-input reproducibility
        if (tcfg.file_path.empty() && (!cfg.record_input.empty() || !cfg.replay_input.empty())) {
            // no file, but still need sequence etc for in-memory checks
        }
        // An explicitly requested evidence stream that cannot be established
        // is a run-initialization failure, not a warning: abort before guest
        // execution so the caller cannot mistake a run with no recording, no
        // trace, or no replayed input for the requested evidence.
        bool evidence_init_ok = true;
        if (!tcfg.file_path.empty() || tcfg.mirror_stderr || !tcfg.filter.empty() || tcfg.max_frames != 0 || tcfg.enable_enter_exit == false) {
            if (!m2host::trace::trace_init(tcfg)) {
                std::fprintf(stderr,
                             "[trace] requested trace could not be initialized: %s\n",
                             tcfg.file_path.empty() ? "(stderr)"
                                                    : tcfg.file_path.c_str());
                evidence_init_ok = false;
            } else {
                std::printf("[trace] harness active: file=%s frames=%u filter=%s no_enter_exit=%d\n",
                            tcfg.file_path.empty()?"(none)":tcfg.file_path.c_str(),
                            tcfg.max_frames, cfg.trace_filter.empty()?"all":cfg.trace_filter.c_str(),
                            int(cfg.trace_no_enter_exit));
            }
        } else if (!cfg.trace_file.empty()) {
            evidence_init_ok = m2host::trace::trace_init(tcfg);
        }
        if (!cfg.record_input.empty() || !cfg.replay_input.empty()) {
            evidence_init_ok =
                m2host::trace::recorder_init(cfg.record_input, cfg.replay_input) &&
                evidence_init_ok;
        }
        // Broker recorder (discrete EventBroker)
        if (!cfg.record_broker.empty() || !cfg.replay_broker.empty()) {
            evidence_init_ok =
                m2host::trace::broker_recorder_init(cfg.record_broker,
                                                    cfg.replay_broker) &&
                evidence_init_ok;
        }
        if (!evidence_init_ok) {
            m2host::trace::input_recorder().stop_recording();
            m2host::trace::input_recorder().stop_replay();
            m2host::trace::input_recorder().stop_broker_recording();
            m2host::trace::input_recorder().stop_broker_replay();
            std::fprintf(stderr,
                         "[main] requested input/broker evidence initialization "
                         "failed; aborting before guest execution\n");
            return 3;
        }
        // Emit boot state snapshot — also covers case where trace was not requested but we still want reproducibility
        // Defer to after guest context is ready; here we just note boot intent.
    }

    std::printf("=== IMSA M2 guest host ===\n");
    std::printf("[main] data root: %s%s\n",
                cfg.root.empty() ? "(not found)" : m2host::platform::path_to_utf8(cfg.root).c_str(),
                cfg.root.empty() ? "" : "");

    // Headless keeps the null video backend, but M2_FORCE_MIXER=1 opens the
    // SDL mixer anyway (dummy audio driver where no device exists) so the
    // authentic guest->contract->AudioShim->mixer chain runs without a
    // display. Default behavior is unchanged.
    bool force_mixer = false;
    bool disable_mixer = false;
    {
        const char *dm = std::getenv("M2_DISABLE_MIXER");
        disable_mixer = dm && *dm && *dm != '0';
    }
    if (cfg.headless) {
        const char *fm = std::getenv("M2_FORCE_MIXER");
        force_mixer = fm && *fm && *fm != '0';
        if (force_mixer && !SDL_WasInit(SDL_INIT_AUDIO))
            SDL_InitSubSystem(SDL_INIT_AUDIO);
    }
    // The launcher owns the foreground while the child game window opens. Keep
    // controller events arriving until the window takes focus, matching the
    // launcher's own capture path.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!cfg.headless && !SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // --- host services (LaunchMe steps 1–6 mapped, see recomp_entry.c) --------
    m2host::FilesystemShim filesystem;
    filesystem.init(cfg.root);

    m2host::EventBroker broker;
    broker.init(cfg.headless);

    m2host::AudioShim audio;
    audio.init((cfg.headless && !force_mixer) || disable_mixer, !cfg.control_socket.empty());
    if (disable_mixer)
        std::printf("[audio] diagnostic mixer disable active (M2_DISABLE_MIXER)\n");
    {
        // M2_AUDIO_MUTE: silence the open playback stream without changing the
        // device-backed timing path, so unattended runs on a shared machine
        // make no sound while the asynchronous DSPP worker still owns the
        // production audio pipeline.
        const char* mute = std::getenv("M2_AUDIO_MUTE");
        if (mute && *mute && *mute != '0') {
            audio.set_output_muted(true);
            std::printf("[audio] output muted (M2_AUDIO_MUTE); device timing path unchanged\n");
        }
    }

    // Optional OGG music library (--music-dir). Off unless a folder is given;
    // the original CBD2 music path is unaffected.
    if (!cfg.music_dir.empty())
        m2host::audio_ogg::scan_library(cfg.music_dir);

    m2host::MpegDecoder mpeg;
    if (mpeg.open()) std::printf("[main] mpegaudiodecoder shim armed\n");

    signal(SIGINT, host_sigint_handler);
    m2host::folio_install_all();
    m2host::input::input_shim_register();  // kernel-port surface
    std::printf("[boot] LaunchMe-equivalent services ready\n");
#ifdef M2_HAVE_RECOMP
    std::printf("[boot] game entry: recomp-linked libm2recomp (game_data.c + game_text_*.c)\n");
#else
    std::printf("[boot] game entry: STUB (configure with -DIMSA_DISC or -DIMSA_DATA_DIR to build the game)\n");
#endif

    // --- settings ---------------------------------------------------------------
    // Choose the guest-facing controller device before the Event Broker builds
    // its PodStateTable. This is independent of the physical bindings profile.
#ifdef M2_HAVE_RECOMP
    m2_controls_set_mode(cfg.controls_mode);
    m2_analog_steering_set_enabled(cfg.improved_analog_steering);
#endif
    m2_guest_set_controller_type(cfg.controls_mode == M2_CONTROLS_ORIGINAL && cfg.analog_controller ? 1 : 0);
    m2host::input::InputMap driving_input;
    driving_input.set_custom_controls(cfg.controls_mode == M2_CONTROLS_CUSTOM);
    m2host::input::HostControls host_controls;
    const bool replay_mode = !cfg.replay_input.empty() || !cfg.replay_broker.empty();
    // Physical bindings come from --controls-profile, or the launcher's
    // keyboard defaults without one. Replay never reads physical input.
    if (!replay_mode) {
        std::string error;
        if (cfg.controls_profile.empty()) host_controls.defaults();
        else if (!host_controls.load(m2host::platform::path_from_utf8(cfg.controls_profile), error)) {
            std::fprintf(stderr, "[controls] %s\n", error.c_str());
            return 2;
        }
        if (!cfg.headless) host_controls.open_devices();
    }
    m2host::Config settings;
    {
        settings.set_path(host_config_path("m2host.cfg"));
        settings.load();  // missing file keeps defaults
    }
    m2host::input::broker_bridge_set_control_port(
        &driving_input.control_port(0));

    // --- graphics ---------------------------------------------------------------
    std::unique_ptr<m2host::ICelBlitterBackend> gfx;
    // Owned by gfx for its whole lifetime; used to turn a failed explicit
    // --capture output into a nonzero process status after the frame pump.
    m2host::SoftCaptureBackend* soft_capture = nullptr;
    SDL_Window* window = nullptr;

    if (cfg.headless) {
        if (!cfg.capture_dir.empty()) {
            // Headless frame capture uses the deterministic software backend
            // (NullBlitter has no pixels to dump).
            auto backend = std::make_unique<m2host::SoftCaptureBackend>(
                cfg.width, cfg.height, cfg.capture_dir);
            soft_capture = backend.get();
            gfx = std::move(backend);
            std::printf("[gfx] headless: software capture backend -> %s\n",
                        m2host::platform::path_to_utf8(cfg.capture_dir).c_str());
        } else if (!cfg.screenshot_path.empty() || !cfg.control_socket.empty()) {
            // Headless screenshot uses software backend with frame dumping disabled
            gfx = std::make_unique<m2host::SoftCaptureBackend>(
                cfg.width, cfg.height, std::filesystem::path{});
            std::printf("[gfx] headless: software backend (screenshot -> %s)\n",
                        m2host::platform::path_to_utf8(cfg.screenshot_path).c_str());
        } else {
            gfx = std::make_unique<m2host::NullBlitter>();
            std::printf("[gfx] headless: null backend\n");
        }
    } else {
#if defined(M2_HAVE_GL)
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);  // clean-renderer shadows
#if defined(__APPLE__)
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#else
        // Both renderers use only GL 3.3 core entry points and #version 330
        // shaders. Asking for more makes context creation fail on Mesa/older
        // GPUs/VMs, which silently drops to the null (black) backend.
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif
        // M2_RENDER_SCALE fixes the internal resolution, so the drawable only
        // decides output sharpness: ask for the display's full pixel density
        // (Retina/HiDPI) instead of a point-sized drawable the compositor
        // stretches. Without a render scale the targets follow the drawable,
        // and keeping it point-sized keeps the original cost.
        SDL_WindowFlags window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
        if (m2host::render_scale_request() > 0)
            window_flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
        window = SDL_CreateWindow("IMSA M2 — recomp host", cfg.width, cfg.height,
                                  window_flags);
        if (!window) {
            SDL_Log("window creation failed: %s", SDL_GetError());
            return 1;
        }
        {
            const std::filesystem::path icon_path = m2host::platform::find_runtime_icon();
            if (!icon_path.empty()) {
                SDL_Surface* icon_surf = SDL_LoadPNG(icon_path.string().c_str());
                if (icon_surf) {
                    SDL_SetWindowIcon(window, icon_surf);
                    SDL_DestroySurface(icon_surf);
                }
                m2host::platform::apply_platform_app_icon(icon_path);
            }
        }
        // The recovered image is 4:3 (640x480 display, 576x432 carve). Lock the
        // window's content aspect so an interactive resize or zoom cannot
        // stretch it. The request can be declined by the windowing system, so
        // report either outcome; a non-4:3 --width/--height is a deliberate
        // diagnostic exception and is called out because it does stretch.
        if (SDL_SetWindowAspectRatio(window, 4.f / 3.f, 4.f / 3.f))
            std::printf("[video] window aspect locked to 4:3\n");
        else
            std::fprintf(stderr, "[video] window aspect lock unavailable: %s\n",
                         SDL_GetError());
        if (cfg.height > 0 && cfg.width * 3 != cfg.height * 4)
            std::fprintf(stderr,
                         "[video] requested %dx%d is not the game's 4:3 aspect; "
                         "the rendered image is stretched\n",
                         cfg.width, cfg.height);
        // Windowed presentation is placed by the host, not by the guest: a
        // multi-display machine otherwise always opens on the primary display.
        // M2_DISPLAY_INDEX selects an SDL display index for the window (the
        // built-in panel on a laptop beside an external monitor), and the
        // display list is reported so the index is an observation rather than a
        // guess. Unset keeps the platform default position.
        {
            int display_count = 0;
            SDL_DisplayID* displays = SDL_GetDisplays(&display_count);
            if (displays != nullptr && display_count > 0) {
                std::string list;
                for (int i = 0; i < display_count; ++i) {
                    SDL_Rect bounds{};
                    const bool have_bounds =
                        SDL_GetDisplayBounds(displays[i], &bounds);
                    const char* name = SDL_GetDisplayName(displays[i]);
                    list += std::to_string(i) + "=" + (name ? name : "?") +
                            "(" + std::to_string(have_bounds ? bounds.w : 0) +
                            "x" + std::to_string(have_bounds ? bounds.h : 0) +
                            "@" + std::to_string(have_bounds ? bounds.x : 0) +
                            "," + std::to_string(have_bounds ? bounds.y : 0) +
                            ")" + (i + 1 < display_count ? " " : "");
                }
                std::printf("[video] displays: %s\n", list.c_str());
                const char* display_env = std::getenv("M2_DISPLAY_INDEX");
                const int display_index =
                    (display_env && *display_env)
                        ? static_cast<int>(std::strtol(display_env, nullptr, 0))
                        : -1;
                if (display_index >= display_count) {
                    std::fprintf(stderr,
                                 "[video] M2_DISPLAY_INDEX=%d is out of range "
                                 "(0..%d); keeping the platform default\n",
                                 display_index, display_count - 1);
                } else if (display_index >= 0) {
                    SDL_Rect bounds{};
                    if (SDL_GetDisplayBounds(displays[display_index], &bounds)) {
                        // Centre on the selected display; bounds are in the
                        // global desktop coordinate space SDL reports.
                        const int x = bounds.x +
                                      (bounds.w - cfg.width) / 2;
                        const int y = bounds.y +
                                      (bounds.h - cfg.height) / 2;
                        SDL_SetWindowPosition(window, x, y);
                        std::printf(
                            "[video] window on display %d (%s) at %d,%d\n",
                            display_index,
                            SDL_GetDisplayName(displays[display_index]), x,
                            y);
                    }
                }
            }
            if (displays != nullptr) SDL_free(displays);
        }
        // Fullscreen is requested after the window is placed so it opens on
        // the chosen display. The renderers fit the 4:3 image inside the
        // fullscreen drawable (black bars) by reading SDL_WINDOW_FULLSCREEN.
        if (cfg.fullscreen || cfg.borderless_fullscreen) {
            bool exclusive = !cfg.borderless_fullscreen;
            if (exclusive) {
                // Keep the desktop's current mode: no display mode switch, so
                // the display's native pixels are used for the fitted image.
                const SDL_DisplayMode* desktop =
                    SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(window));
                if (!desktop || !SDL_SetWindowFullscreenMode(window, desktop)) {
                    std::fprintf(stderr,
                                 "[video] exclusive fullscreen unavailable (%s); "
                                 "using borderless fullscreen\n",
                                 SDL_GetError());
                    exclusive = false;
                }
            }
            if (!exclusive) SDL_SetWindowFullscreenMode(window, nullptr);
            if (SDL_SetWindowFullscreen(window, true)) {
                SDL_SyncWindow(window);
                SDL_HideCursor();
                int pw = 0, ph = 0;
                SDL_GetWindowSizeInPixels(window, &pw, &ph);
                std::printf("[video] %s fullscreen %dx%d px\n",
                            exclusive ? "exclusive" : "borderless", pw, ph);
            } else {
                std::fprintf(stderr, "[video] fullscreen failed: %s; staying windowed\n",
                             SDL_GetError());
            }
        }
        // Clean renderer preferred; gl_backend stays as fallback.
        // M2_GL_BACKEND=null keeps the window/input/guest path but installs the
        // no-op backend so CLT interpretation cost can be attributed separately
        // from GL submission (diagnostic only; never a rendering mode).
        const char* gl_backend = std::getenv("M2_GL_BACKEND");
        if (gl_backend && std::strcmp(gl_backend, "null") == 0) {
            gfx = std::make_unique<m2host::NullBlitter>();
        } else if (!gl_backend || std::strcmp(gl_backend, "cel") != 0) {
            gfx = m2host::make_clean_renderer();
        }
        if (!gfx || !gfx->init(window)) {
            std::printf("[gfx] clean renderer unavailable; falling back to cel blitter\n");
            if (gfx) gfx->shutdown();
            gfx = m2host::make_gl_backend();
            if (!gfx || !gfx->init(window)) {
                SDL_Log("GL backend unavailable; continuing with null backend");
                gfx = std::make_unique<m2host::NullBlitter>();
            }
        }
        // Report the renderer selected by the graphics driver.
        if (SDL_GL_GetCurrentContext()) {
#if defined(_WIN32)
            using GetStringFn = const unsigned char*(__stdcall*)(unsigned);
#else
            using GetStringFn = const unsigned char* (*)(unsigned);
#endif
            const auto get_string =
                reinterpret_cast<GetStringFn>(SDL_GL_GetProcAddress("glGetString"));
            const auto gl_text = [&](unsigned name) {
                const unsigned char* text = get_string ? get_string(name) : nullptr;
                return text ? std::string(reinterpret_cast<const char*>(text)) : std::string();
            };
            const std::string gl_vendor = gl_text(0x1F00);    // GL_VENDOR
            const std::string gl_renderer = gl_text(0x1F01);  // GL_RENDERER
            std::printf("[gpu] OpenGL renderer: %s (%s)\n", gl_renderer.c_str(), gl_vendor.c_str());
        }
        // Unattended GL diagnostics may run without display-link callbacks.
        // Disabling swap synchronization must not disable the guest frame
        // completion barrier below (unlike --fast-forward).
        // M2_VBL_PACED_SWAP paces the interactive loop on the rational NTSC VBL
        // deadline: the swap must not block (swap interval 0),
        // so the guest's field cadence no longer inherits a missed display
        // compositor swap. An explicit M2_GL_SWAP_INTERVAL still wins.
        const char* swap_interval = std::getenv("M2_GL_SWAP_INTERVAL");
        const bool vbl_paced_swap = cfg.vbl_paced_swap;
        SDL_GL_SetSwapInterval(swap_interval ? std::atoi(swap_interval)
                                             : (vbl_paced_swap ? 0 : 1));
#else
        gfx = std::make_unique<m2host::NullBlitter>();
        std::printf("[gfx] built without GL: null backend\n");
#endif
    }

    // The guest owns CLT words and submits them through the TE IO path.  The
    // bridge copies those words on the guest callback and executes them only
    // from this host/render thread.
    m2host::GuestCltBridge guest_clt(gfx.get());
    const char* persist_target_env = std::getenv("M2_CLT_PERSIST_TARGET");
    const bool persist_guest_targets =
        !persist_target_env || persist_target_env[0] == '\0' ||
        persist_target_env[0] != '0';
    gfx->clt_target_persistence(persist_guest_targets);
    guest_clt.set_target_persistence(persist_guest_targets);
    // The look-ahead in-flight pump is limited to the video-overlay lifecycle,
    // whose owning application frame never completes. Set
    // M2_CLT_INFLIGHT_OVERLAY=0 to restore the general look-ahead pump over
    // every scene as the falsification control.
    const char* inflight_overlay_env = std::getenv("M2_CLT_INFLIGHT_OVERLAY");
    const bool inflight_overlay_only =
        inflight_overlay_env == nullptr || inflight_overlay_env[0] == '\0' ||
        inflight_overlay_env[0] != '0';
    guest_clt.set_inflight_overlay_only(inflight_overlay_only);
    m2host::DiagnosticControl diagnostic;
    if (!cfg.control_socket.empty()) {
        std::string error;
        if (!diagnostic.open(cfg.control_socket, &guest_clt, &audio,
                             [&](const std::filesystem::path& path) {
                                 return save_ppm_screenshot(gfx.get(), path);
                             }, error)) {
            std::fprintf(stderr, "[control] %s\n", error.c_str());
            return 2;
        }
    }

    // Pace the application when it returns to its loop head, while keeping
    // platform display/IO/audio service independent of that loop. A guest can
    // play an entire movie inside one application iteration.
    const bool capture_pacing =
        cfg.headless && (!cfg.capture_dir.empty() || !cfg.screenshot_path.empty() || diagnostic.enabled());
    const bool interactive_pacing = !cfg.headless && !cfg.fast_forward;
    const bool frame_pacing = capture_pacing || interactive_pacing || diagnostic.enabled();
#ifdef M2_HAVE_RECOMP
    m2_guest_frame_pacing_set(frame_pacing ? 1 : 0);
    if (capture_pacing) {
        std::printf("[gfx] capture pacing: guest frame permits, independent platform service\n");
    } else if (interactive_pacing) {
        std::printf("[gfx] interactive pacing: guest frame permits, independent platform service\n");
    }
#endif

    // --- synthetic boot or checkpoint restore boundary --------------------------
    // A restore must initialize the guest image/kernel without first spawning
    // a fresh main task. The M2EX loader attaches only the task records whose
    // STARTED state or explicit lifted continuation has been proven.
    int boot_rc = 0;
#ifdef M2_HAVE_RECOMP
    if (!cfg.savestate_load.empty()) {
        std::printf("[boot] checkpoint load requested: preparing restore without guest boot task\n");
        boot_rc = m2_guest_prepare_restore();
        if (boot_rc != 0)
            std::fprintf(stderr, "[boot] checkpoint restore preparation failed rc=%d\n", boot_rc);
    } else {
        std::printf("[boot] calling m2_game_entry()\n");
        boot_rc = m2_game_entry();
        std::printf("[boot] m2_game_entry returned rc=%d\n", boot_rc);
    }
#else
    std::printf("[boot] calling m2_game_entry()\n");
    boot_rc = m2_game_entry();
    std::printf("[boot] m2_game_entry returned rc=%d\n", boot_rc);
#endif

    // Post-entry folio report: the typed guest kernel registry went live
    // during the guest boot or checkpoint-restore preparation, so selectors
    // it owns no longer read as host stubs. Without recomp the probe stays
    // unset and the host table speaks.
#ifdef M2_HAVE_RECOMP
    m2host::folio_set_guest_probe(&m2host::guest_registry_probe);
    m2host::folio_set_observed_probe(&m2host::guest_observed_probe);
#endif
    // The two images import different folio sets, so name selectors out of
    // the register of the image that actually booted.
    m2host::folio_set_name_source(boot_shooter2d);
    m2host::folio_print_report(stdout);
#ifdef M2_HAVE_RECOMP
    // Guest heartbeat: the gate expects a [guest] line strictly after
    // the handoff line. The guest thread itself prints [guest] task ... but
    // scheduling may place it before the handoff. Emit a host-observed
    // heartbeat after the report so ordering is deterministic.
    std::printf("[guest] host observed handoff — StateDriver owns loop (frame pump starting)\n");
#endif

    // The original DSPP advances from the 44.1 kHz audio clock independently
    // of VBL. Interactive playback services that platform clock on a bounded
    // worker instead of serializing its interpreter with rendering. The audio
    // graph lock preserves selector/mutation ordering, and each service call
    // is capped at 128 frames so guest audio calls can interleave with catchup.
    // Headless and diagnostic-control runs retain their deterministic
    // synchronous host-boundary service.
    std::atomic<bool> dspp_worker_stop{false};
    std::thread dspp_worker;
    const bool async_dspp = audio.device_open() && !cfg.headless &&
                            !diagnostic.enabled() && !disable_mixer;
    if (async_dspp) {
        // Hand output production to the worker, and queue the startup reserve
        // before the worker's first fill so it covers the first device blocks.
        m2_guest_audio_set_async_dspp(1);
        const uint32_t reserve = audio.prime_dspp_output();
        std::printf("[audio] DSPP startup playback reserve=%u frames\n", reserve);
        dspp_worker = std::thread([&] {
            // This producer supplies a device deadline, independently of
            // rendering. Normal scheduler priority lets CPU-heavy frames
            // starve the DSP FIFO even while the audio callback keeps running.
            if (!SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH))
                std::fprintf(stderr, "[audio] DSPP priority unavailable: %s\n", SDL_GetError());
            while (!dspp_worker_stop.load(std::memory_order_acquire)) {
                if (m2_guest_audio_pump_dspp_output() == 0u)
                    std::this_thread::sleep_for(std::chrono::microseconds(250));
            }
        });
        std::printf("[audio] DSPP audio-clock worker active (128-frame bound)\n");
    }

    // --- frame pump -------------------------------------------------------------
    int ran_frames = 0;
    int exit_code = 0;
    if (boot_rc != 0) {
        // A failed entry has no guest to pump. Report it in the process status
        // so the launcher and CI can tell a broken install from a short run.
        std::fprintf(stderr,
                     "[boot] guest entry failed rc=%d; skipping the frame pump\n",
                     boot_rc);
        exit_code = boot_rc > 0 ? boot_rc : 1;
    }
    uint64_t final_guest_frames = 0;
    bool quit = false;
#ifdef M2_HAVE_RECOMP
    // Interactive streaming presentation: the pump executes only guest frames
    // that have completed, while the guest is permitted to run one application
    // frame ahead. Guest compute and host interpretation then overlap the
    // display interval instead of serializing behind it. The counter is the
    // guest-published completed-frame value (0 at boot).
    uint32_t interactive_last_presented = 0;
#endif

#ifdef M2_HAVE_RECOMP
    // Cadence outcome of the pacing wait below, published to the diagnostic
    // control so a `capture` names it. It describes the iteration that
    // presented the image the capture reads: poll() services a capture before
    // this iteration's wait, so the value still belongs to the previous (and
    // presenting) iteration. Observation only; see PresentWait.
    m2host::PresentWait paced_wait_last = m2host::PresentWait::unpaced;
    auto wait_for_paced_frame = [&](uint32_t expected) -> bool {
        if (!frame_pacing) {
            paced_wait_last = m2host::PresentWait::unpaced;
            return true;
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::microseconds(16667);
        for (;;) {
            // A guest frame can publish more lists than the bounded queue can
            // hold before reaching its next loop head. Drain those copies
            // while the guest waits for the next permit.
            guest_clt.pump();
            guest_clt.service_framebuffer_reads();
            if (m2_guest_frame_pacing_completed() >= expected) {
                paced_wait_last = m2host::PresentWait::completed;
                return true;
            }
            int32_t task_state = -1;
            if (m2_guest_main_query(&task_state, nullptr) == 0 &&
                task_state >= 2) {  // M2K_TASK_COMPLETED or M2K_TASK_FAILED
                paced_wait_last = m2host::PresentWait::guest_ended;
                return false;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                // A pending application frame is not a failed frame. Return
                // to SDL and device service at display cadence: VStreamPlay
                // waits for IO completions and audio cues before returning to
                // the outer StateDriver loop. Waiting here for that return
                // starves those producers and the macOS event loop.
                paced_wait_last = m2host::PresentWait::expired;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    };
#endif

    // --- savestate load before pump ---------------------------------------------
    if (!cfg.savestate_load.empty()) {
#ifdef M2_HAVE_RECOMP
        auto lr = m2host::savestate::load_from_file(cfg.savestate_load);
        if (!lr.ok) {
            std::fprintf(stderr, "[savestate] load FAILED %s: %s\n", cfg.savestate_load.c_str(), lr.error.c_str());
            if (exit_code == 0) exit_code = 4;
        } else {
            std::printf("[savestate] loaded %s hash %016llx\n", cfg.savestate_load.c_str(), (unsigned long long)lr.hash);
        }
#else
        std::fprintf(stderr, "[savestate] load requires M2_HAVE_RECOMP\n");
#endif
    }
    // Developer guest-memory writes (--poke): apply after m2_game_entry and
    // savestate load.
#ifdef M2_HAVE_RECOMP
    if (!cfg.pokes.empty()) {
        PPCContext* pctx = m2_recomp_context();
        for (auto &pr : cfg.pokes) {
            uint32_t ea = pr.first, val = pr.second;
            if (pctx && pctx->mem && ea + 4 <= RECOMP_GUEST_MEM) {
                wr32(pctx, ea, val);
                std::printf("[poke] wrote ea=%#x val=%#x (%u)\n", ea, val, val);
            } else {
                std::fprintf(stderr, "[poke] failed ea=%#x OOB\n", ea);
            }
        }
    }
#endif

    {
        // The recovered guest StateDriver owns application frames. The host
        // supplies window events, input polling, VBL pacing, and snapshots.
        std::printf("[guest] guest StateDriver owns application frames (host supplies VBL/input only)\n");
#ifdef M2_HAVE_RECOMP
        // Verify guest task exists before entering frame pump.
        {
            int32_t _st = -1, _st2 = 0;
            if (m2_guest_main_query(&_st, &_st2) != 0) {
                std::fprintf(stderr, "[guest_gate] FAIL guest task not live at entry — boot failed\n");
                if (exit_code == 0) exit_code = 4;
            }
        }
#endif
        m2host::input::SdlInputAdapter sdl_input;
#ifdef M2_HAVE_RECOMP
        // Opt-in state-driven reproduction driver (M2_PREVIEW_MODE).
        preview_driver_init();
        // Opt-in interactive shot sink (M2_SHOT_DIR/M2_SHOT_FRAMES).
        ShotSink::instance().init();
        // Opt-in display-carve content dump (M2_CLT_CARVE_DUMP).
        CarveDumpSink::instance().init();
#endif
        // Opt-in F12 PNG capture (M2_SCREENSHOT_KEY); needs a window.
        if (!cfg.headless) HotkeyShot::instance().init();
        if (diagnostic.enabled() && !diagnostic.suspend()) {
            std::fprintf(stderr, "[control] initial guest pause failed\n");
            return 4;
        }
        // Bounded frame-cadence attribution (`M2_FRAME_TIMING`): splits each
        // host iteration into the guest pacing wait, CLT pump, and
        // present/swap phases so a below-60 Hz interactive cadence is measured
        // instead of inferred. The value is an optional guest-frame floor;
        // the log is capped and never alters pacing, input, or guest state.
        const char* frame_timing_env = std::getenv("M2_FRAME_TIMING");
        const bool frame_timing_enabled = frame_timing_env != nullptr;
        // The per-iteration log and summary below run only in game-enabled
        // builds, hence [[maybe_unused]] on their state.
        [[maybe_unused]] const uint32_t frame_timing_floor =
            frame_timing_enabled
                ? static_cast<uint32_t>(std::strtoul(frame_timing_env, nullptr, 0))
                : 0u;
        [[maybe_unused]] uint32_t frame_timing_logged = 0;
        auto frame_timing_now = []() -> uint64_t {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
        };
        // Bounded end-of-run cadence summary (M2_FRAME_TIMING). Per-iteration
        // samples are collected only over the post-floor window so boot
        // transients cannot be mistaken for a steady-state miss; the summary
        // is a single line written on exit. Pure observation: sampling never alters pacing, input, or guest state.
        // Host VBL generator for windowed interactive play. The vendor BDA
        // projector is 59.94 fields/second, so VblPacer keeps the exact
        // 60000/1001 rational period and does not accumulate a one/60-second
        // rounding error. A display that refreshes faster must not advance
        // the guest faster (the timing model); capture/diagnostic
        // pacing keeps one signal per host iteration so headless frame gates
        // stay deterministic.
        [[maybe_unused]] m2host::timing::VblPacer interactive_vbl;
        constexpr uint32_t kFrameTimingMaxSamples = 20000u;
        std::vector<float> ft_iter_ms, ft_wait_ms, ft_pump_ms, ft_present_ms,
            ft_audio_ms;
        ft_iter_ms.reserve(kFrameTimingMaxSamples);
        ft_wait_ms.reserve(kFrameTimingMaxSamples);
        ft_pump_ms.reserve(kFrameTimingMaxSamples);
        ft_present_ms.reserve(kFrameTimingMaxSamples);
        ft_audio_ms.reserve(kFrameTimingMaxSamples);
        uint32_t ft_over_16_67 = 0;
        uint32_t ft_multi_frame = 0;
        uint64_t ft_first_time = 0, ft_last_time = 0;
        uint32_t ft_guest_first = 0, ft_guest_last = 0;
        [[maybe_unused]] bool ft_have = false;
        std::chrono::steady_clock::time_point input_replay_origin;
        bool input_replay_started = false;
        while (!quit && boot_rc == 0 &&
               (cfg.frames == 0 || ran_frames < cfg.frames)) {
            if (diagnostic.enabled() && !diagnostic.poll()) {
                if (!cfg.headless) sdl_input.poll_sdl(driving_input, host_controls, true);
                quit = diagnostic.quit_requested() || sdl_input.close_requested() ||
                       s_quit_requested.load(std::memory_order_relaxed);
                continue;
            }
            const uint64_t timing_iter_start =
                frame_timing_enabled ? frame_timing_now() : 0;
            [[maybe_unused]] uint64_t wait_ns = 0;
            [[maybe_unused]] uint64_t pump_ns = 0;
            [[maybe_unused]] uint64_t present_ns = 0;
            [[maybe_unused]] uint64_t audio_ns = 0;
            // Optional State 2 correlation boundary. This remains separate
            // from backend internals so pacing-drain pumps are attributed to
            // the same host presentation frame as the final pump.
            guest_clt.host_frame_begin(static_cast<uint32_t>(ran_frames), true);
            driving_input.begin_frame();
#ifdef M2_HAVE_RECOMP
            driving_input.set_shooter_active(m2_guest_shooter_active() != 0);
#endif
            if (!cfg.headless) sdl_input.poll_sdl(driving_input, host_controls,
                replay_mode || diagnostic.enabled());
            if (sdl_input.close_requested()) quit = true;
            // Replay: inject recorded input after live polling so replay overrides live.
            if (m2host::trace::recorder_has_replay()) {
                const auto input_now = std::chrono::steady_clock::now();
                if (!input_replay_started) {
                    input_replay_origin = input_now;
                    input_replay_started = true;
                }
                const uint64_t input_elapsed_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        input_now - input_replay_origin).count();
                uint32_t cur_gf = 0;
#ifdef M2_HAVE_RECOMP
                uint32_t _seq0=0,_pc0=0,_st0=0,_snap0[4]={0};
                if (m2_guest_frame_query(&_seq0,&_pc0,&_st0,_snap0,4)==0) cur_gf=_seq0;
#endif
                m2host::trace::InputRecord rec2;
#ifndef M2_HAVE_RECOMP
                bool popped2 = false;
#endif
                while (cfg.replay_input_host_time
                           ? m2host::trace::input_recorder().pop_for_host_time(input_elapsed_ms, &rec2)
                           : m2host::trace::input_recorder().pop_for_frame(cur_gf, &rec2)) {
                    driving_input.inject_replay(rec2.buttons, rec2.pressed, rec2.released,
                                               rec2.stick_x, rec2.stick_y, rec2.device);
                    m2host::trace::trace_input(rec2.buttons, rec2.pressed, rec2.released,
                                              rec2.stick_x, rec2.stick_y, rec2.device, cur_gf);
#ifndef M2_HAVE_RECOMP
                    popped2 = true;
#endif
                }
#ifndef M2_HAVE_RECOMP
                if (!popped2 && !cfg.replay_input_host_time) {
                    if (m2host::trace::input_recorder().pop_next(&rec2)) {
                        driving_input.inject_replay(rec2.buttons, rec2.pressed, rec2.released,
                                                   rec2.stick_x, rec2.stick_y, rec2.device);
                        m2host::trace::trace_input(rec2.buttons, rec2.pressed, rec2.released,
                                                  rec2.stick_x, rec2.stick_y, rec2.device, cur_gf);
                    }
                }
#endif
            }
            // Broker replay (discrete)
            if (m2host::trace::recorder_has_broker_replay()) {
                uint32_t cur_gb = 0;
#ifdef M2_HAVE_RECOMP
                uint32_t _sb2=0,_pcb2=0,_stb2=0,_snapb2[4]={0};
                if (m2_guest_frame_query(&_sb2,&_pcb2,&_stb2,_snapb2,4)==0) cur_gb=_sb2;
#endif
                m2host::trace::BrokerRecord br2;
#ifndef M2_HAVE_RECOMP
                bool popped_b2 = false;
#endif
                while (m2host::trace::input_recorder().has_next_broker_for_frame(cur_gb) &&
                       m2host::trace::input_recorder().pop_broker_for_frame(cur_gb, &br2)) {
                    m2host::M2EventRecord ev{}; ev.what = br2.what; ev.x = br2.x; ev.y = br2.y; ev.data = br2.data;
                    broker.inject(ev);
                    {
                        char buf[256]; std::snprintf(buf,sizeof(buf),
                            "{\"broker_what\":%u,\"broker_x\":%d,\"broker_y\":%d,\"broker_data\":%u}",
                            (unsigned)br2.what, br2.x, br2.y, br2.data);
                        m2host::trace::trace_emit(m2host::trace::EventKind::Input, 0, 0, cur_gb, buf);
                    }
#ifndef M2_HAVE_RECOMP
                    popped_b2 = true;
#endif
                }
#ifndef M2_HAVE_RECOMP
                if (!popped_b2) {
                    if (m2host::trace::input_recorder().pop_next_broker(&br2)) {
                        m2host::M2EventRecord ev{}; ev.what = br2.what; ev.x = br2.x; ev.y = br2.y; ev.data = br2.data;
                        broker.inject(ev);
                        char buf[256]; std::snprintf(buf,sizeof(buf),
                            "{\"broker_what\":%u,\"broker_x\":%d,\"broker_y\":%d,\"broker_data\":%u}",
                            (unsigned)br2.what, br2.x, br2.y, br2.data);
                        m2host::trace::trace_emit(m2host::trace::EventKind::Input, 0, 0, cur_gb, buf);
                    }
                }
#endif
            }
#ifdef M2_HAVE_RECOMP
            // Frame sync steps 1-2: host polls platform input (above) and
            // writes bounded guest-visible snapshot to guest event broker buffer.
            // Step 3 is the VBL signal that unblocks guest WaitSignal(mask 0x10).
            uint32_t paced_frame_before = 0;
            bool have_paced_frame_before = false;
            uint32_t paced_state = 0;
            uint32_t paced_snap[4] = {0, 0, 0, 0};
            if (frame_pacing || preview_driver().active) {
                // The state-driven preview driver needs the observed guest
                // frame/state even when frame pacing is off (headless or
                // --fast-forward), otherwise it would only ever see seq=0.
                uint32_t paced_pc = 0;
                have_paced_frame_before =
                    m2_guest_frame_query(&paced_frame_before, &paced_pc,
                                         &paced_state, paced_snap, 4) == 0;
            }
            {
                const auto& cp0 = driving_input.control_port(0);
                if (!diagnostic.enabled()) {
                    if (cfg.controls_mode != M2_CONTROLS_ORIGINAL || cfg.improved_analog_steering) m2_controls_publish(
                        cfg.controls_mode == M2_CONTROLS_CUSTOM ? cp0.custom_steer_analog : cp0.raw_steer,
                        cfg.controls_mode == M2_CONTROLS_CUSTOM ? cp0.custom_steer_digital : 0.f,
                        cfg.controls_mode == M2_CONTROLS_CUSTOM ? cp0.custom_throttle : cp0.raw_throttle,
                        cfg.controls_mode == M2_CONTROLS_CUSTOM ? cp0.custom_brake : cp0.raw_brake);
                    m2_guest_input_set_cport_edges(cp0.buttons, cp0.pressed,
                                                   cp0.stick_x, cp0.stick_y);
                } else {
                    m2_diagnostic_input_publish();
                }
            }
#ifdef M2_HAVE_RECOMP
            if (preview_driver().active) {
                const uint32_t guest_seq = have_paced_frame_before
                                               ? paced_frame_before : 0;
                const uint32_t guest_state =
                    have_paced_frame_before ? paced_state : 0;
                uint32_t gbuttons = 0;
                uint8_t gsx = 128, gsy = 128;
                const int r = m2_drv_tick(&preview_driver().drv, guest_seq,
                                          guest_state, paced_snap[0],
                                          &gbuttons, &gsx, &gsy);
                // The driver answers with input for the NEXT guest frame;
                // r > 0: script actively pressed buttons
                // r == 0: script actively running (waiting), keep neutral
                // r < 0: script completed
                if (r > 0) {
                    m2_guest_input_set_cport(gbuttons, gsx, gsy);
                } else if (r == 0) {
                    m2_guest_input_set_cport(0, 128, 128);
                } else {
                    if (preview_driver().has_tail) {
                        m2_guest_input_set_cport(preview_driver().tail_buttons,
                                                 preview_driver().tail_sx,
                                                 preview_driver().tail_sy);
                    }
                    // If no tail, cp0 set at line 837 is preserved
                }
            }
#endif
            if (frame_pacing && !interactive_pacing) {
                m2host::perf::Scope renderer_scope(
                    m2host::perf::Phase::RendererWork);
                gfx->frame_begin(0.05f, 0.07f, 0.10f);
            }
            // One emulated projector field owns both the StateDriver VBL and
            // visibility of a queued DISPLAYFRAMEBUFFER. Keep this outside
            // the wait scope so a 120 Hz host swap cannot acknowledge two M2
            // display units inside one 59.94 Hz field.
            bool display_field_due = true;
            {
                const uint64_t timing_wait_start =
                    frame_timing_enabled ? frame_timing_now() : 0;
                m2host::perf::Scope guest_frame_wait(
                    m2host::perf::Phase::GuestFrameWait);
                // Host VBL generator for windowed interactive play. The M2's
                // vertical blank is the NTSC 60 Hz field rate and the whole
                // application is VBL-locked, so a display that refreshes
                // faster must not advance the guest faster
                // (the timing model); the pump below still
                // consumes exactly the frames the guest has published.
                // Diagnostic-control runs keep their deterministic
                // one-tick-one-VBL stepping contract, and headless capture
                // pacing keeps one signal per host iteration.
                if (interactive_pacing && !diagnostic.enabled()) {
                    if (cfg.vbl_paced_swap) {
                        // The swap is non-blocking in this mode, so wait for
                        // the pacer's next field deadline here instead of
                        // inheriting the display's swap timing. Lateness is
                        // the pacer's problem: due() re-anchors and never
                        // replays missed fields as a burst.
                        const uint64_t deadline =
                            interactive_vbl.next_deadline_ns();
                        const uint64_t now_ns = frame_timing_now();
                        if (now_ns < deadline) {
                            const uint64_t remaining_ns = deadline - now_ns;
                            // Sleep for the bulk of the wait, yielding for the last millisecond
                            // to prevent OS scheduler oversleep from slipping past the field boundary.
                            if (remaining_ns > 1000000ull) {
                                std::this_thread::sleep_for(
                                    std::chrono::nanoseconds(remaining_ns - 1000000ull));
                            }
                            while (frame_timing_now() < deadline) {
                                std::this_thread::yield();
                            }
                        }
                    }
                    const uint64_t vbl_now = frame_timing_now();
                    display_field_due = interactive_vbl.due(vbl_now);
                }
                if (display_field_due) (void)m2_guest_signal_vbl();
                if (frame_pacing && !interactive_pacing &&
                    have_paced_frame_before) {
                    m2host::perf::Scope queue_wait(
                        m2host::perf::Phase::CltQueueWait);
                    const uint32_t expected_paced_frame =
                        paced_frame_before + 1u;
                    if (!wait_for_paced_frame(expected_paced_frame)) {
                        if (exit_code == 0) exit_code = 4;
                    }
                }
                if (frame_timing_enabled)
                    wait_ns = frame_timing_now() - timing_wait_start;
            }
#ifdef M2_HAVE_RECOMP
            if (diagnostic.enabled())
                diagnostic.set_present_wait(paced_wait_last);
#endif
            // The original Audio Folio's DSP timer interrupt wakes due Cue
            // events independently of VBL. Pump the bounded equivalent from
            // the host frame boundary using the rendered audio-frame clock.
            const uint64_t timing_audio_start =
                frame_timing_enabled ? frame_timing_now() : 0;
            if (diagnostic.enabled()) audio.diagnostic_tick();
            // With no SDL device, advance the Audio Folio fallback only on
            // the emulated M2 field. A faster host presentation loop must not
            // accelerate Cue/envelope time. Device-backed audio ignores this
            // flag and follows its independent rendered-sample counter.
            m2_guest_audio_pump_timers_for_tick(display_field_due ? 1 : 0);
            if (frame_timing_enabled)
                audio_ns = frame_timing_now() - timing_audio_start;
            // Poll the guest-owned frame completion record (immutable snapshot).
            {
                int32_t st = -1, st2 = 0;
                if (m2_guest_main_query(&st, &st2) == 0) {
                    if (ran_frames % 60 == 0)
                        std::printf("[guest] StateDriver state=%d status=%d host_frame=%d\n", st, st2, ran_frames);
                }
                uint32_t seq = 0, pc = 0, state = 0, snap[4] = {0, 0, 0, 0};
                if (m2_guest_frame_query(&seq, &pc, &state, snap, 4) == 0) {
                    if (ran_frames % 60 == 0) {
                        std::printf("[guest] frame_tick seq=%u pc=0x%x state=%u snap=[%u %u %u %u] host_frame=%d\n",
                                    seq, pc, state, snap[0], snap[1], snap[2], snap[3], ran_frames);
                    }
                    if (seq == 0 && ran_frames >= 60 && ran_frames % 60 == 0) {
                        std::fprintf(stderr,
                                     "[guest_gate] WARN host_frame=%d guest seq=0 state=%u — guest not producing frames\n",
                                     ran_frames, state);
                    }
                }
            }
#endif
            // Frame sync steps 5-6: host services platform events and presents the
            // current guest-owned framebuffer.
            // Guest graphics/audio requests are serviced inline via selector
            // dispatch (shim_abi handlers); the frame pump only presents.
            // SDL is drained only by SdlInputAdapter above. A second pump here
            // stole events arriving during guest execution, including key-up.
            {
                m2host::M2EventRecord ev{};
                while (broker.poll(&ev)) {
                    if (ev.what == m2host::M2_EV_QUIT) quit = true;
                    if (ev.what == m2host::M2_EV_KEY && ev.data == 27) quit = true;
                }
            }
            if (driving_input.quit_requested()) quit = true;
            if (s_quit_requested.load(std::memory_order_relaxed)) quit = true;
            if (!frame_pacing) {
                m2host::perf::Scope renderer_scope(
                    m2host::perf::Phase::RendererWork);
                gfx->frame_begin(0.05f, 0.07f, 0.10f);
            }
#ifdef M2_HAVE_RECOMP
            // Interactive streaming: a completed guest frame starts a fresh
            // target and is the only thing that iteration interprets. A frame
            // that is still being computed stays queued (frame-tagged) and the
            // still-last presented target is re-presented at the next vsync.
            // A completed frame with no CLT work must not clear the target:
            // hardware leaves the display unchanged when the guest submits
            // nothing.
            static bool video_overlay_seeded = false;
            static uint32_t completed_te_scanout = 0;
            uint32_t overlay_view_base = 0, overlay_draw_base = 0;
            const bool video_overlay = m2_guest_video_overlay_targets(
                &overlay_view_base, &overlay_draw_base);
            uint32_t interactive_completed = 0;
            const bool interactive_new_frame =
                interactive_pacing &&
                ((interactive_completed =
                      m2_guest_frame_pacing_completed()) >
                 interactive_last_presented);
            const uint32_t interactive_current =
                interactive_pacing ? m2_guest_current_frame_c() : 0u;
            // General look-ahead service executes a guest frame that is still
            // being built, and its partial lists land in the target presented
            // in this same iteration. M2_CLT_INFLIGHT_OVERLAY limits that
            // service to the video-overlay lifecycle, where the owning
            // application frame never completes.
            const bool interactive_inflight_work =
                guest_clt.inflight_lookahead_allowed(
                    interactive_pacing, video_overlay, interactive_completed,
                    interactive_current);
            const bool interactive_has_work =
                interactive_new_frame &&
                guest_clt.has_pending_upto(interactive_completed);
            // A paced DISPLAYFRAMEBUFFER is a source-defined completion
            // boundary inside a still-running application frame: GS_SendList
            // and GS_WaitTEPass have finished, and the TE driver now waits for
            // this display unit to become visible. Service those published
            // lists before consuming the unit so the guest cannot deadlock at
            // a transition and the selected target cannot be presented stale.
            uint32_t interactive_display_frame = 0;
            const bool interactive_display_ready =
                interactive_pacing &&
                guest_clt.has_display_target_upto(interactive_current,
                                                   &interactive_display_frame);
            const bool interactive_display_work =
                interactive_display_ready &&
                guest_clt.has_pending_upto(interactive_display_frame);
            if (m2host::clt_interactive_frame_begin_needed(
                    interactive_has_work, interactive_display_work)) {
                m2host::perf::Scope renderer_scope(
                    m2host::perf::Phase::RendererWork);
                gfx->frame_begin(0.05f, 0.07f, 0.10f);
            }
            bool pumped_completed = false;
            bool pumped_inflight = false;
#endif
#ifdef M2_HAVE_RECOMP
            static std::vector<uint32_t> display_rgba(
                M2_DISPLAY_WIDTH * M2_DISPLAY_HEIGHT);
#endif
            const uint64_t timing_pump_start =
                frame_timing_enabled ? frame_timing_now() : 0;
#ifdef M2_HAVE_RECOMP
            if (video_overlay && !video_overlay_seeded) {
                // VideoPlayer has already changed the Projector View by the
                // time the stream workers make this lifecycle visible. Use
                // the last completed TE scanout as the pre-switch image; the
                // current View may be an MPEG bitmap and must not leak into
                // the persistent game framebuffers.
                if (!m2host::seed_video_overlay_targets(
                        gfx.get(), completed_te_scanout, overlay_view_base,
                        overlay_draw_base, display_rgba.data(),
                        M2_DISPLAY_WIDTH, M2_DISPLAY_HEIGHT) &&
                    overlay_draw_base != overlay_view_base) {
                    gfx->clt_target_copy(overlay_draw_base,
                                         overlay_view_base);
                }
                video_overlay_seeded = true;
            } else if (!video_overlay) {
                video_overlay_seeded = false;
            }
            uint32_t consumed_target = 0;
            uint32_t consumed_tag = 0;
            uint32_t consumed_boundary = 0;
            const char* consume_mode = interactive_pacing ? "interactive" : "bulk";
            if (interactive_pacing) {
                if (interactive_new_frame && interactive_has_work) {
                    guest_clt.pump(interactive_completed, "completed");
                    pumped_completed = true;
                }
                if (interactive_display_work && !pumped_completed) {
                    // DISPLAYFRAMEBUFFER certifies the published unit's
                    // lists, not the next frame the guest may now be building.
                    // Admitting that newer work shares this unit's depth
                    // reset and can leave a partially rendered next target.
                    guest_clt.pump_display_target_upto(interactive_current);
                    pumped_inflight = true;
                }
                if (interactive_inflight_work) {
                    guest_clt.pump(interactive_current, "inflight");
                    pumped_inflight = true;
                }
                // One display unit per emulated M2 display field, rather than
                // once per completed guest frame or once per host-monitor
                // swap, and
                // bounded by the frame the guest is executing rather than by
                // the last completed one. A guest frame may publish a whole
                // sequence -- the authored state transitions submit one
                // DISPLAYFRAMEBUFFER per animation frame (16, 30 or 48 of them)
                // from inside a single StateDriver iteration -- so draining at
                // the completed-frame cadence would present the sequence
                // correctly but leave a permanent backlog equal to its length,
                // i.e. a standing presentation lag. The publisher is the
                // guest's own synchronous GS_EndFrame, issued after its
                // GS_SendList/GS_WaitTEPass completed the frame's lists, so
                // 'in flight' does not describe it; a unit tagged above the
                // frame the guest has reached is still refused.
                consumed_boundary = interactive_display_ready
                                        ? interactive_display_frame
                                        : interactive_completed;
                // Do not consume a unit that raced in after the readiness
                // sample: it has not participated in this iteration's pump.
                // A newly completed application frame is independently safe,
                // since its completion marker follows all of its display/list
                // publication.
                const uint32_t next_target =
                    display_field_due &&
                            (interactive_display_ready || interactive_new_frame)
                        ? guest_clt.consume_display_target_upto(
                              consumed_boundary, &consumed_tag)
                        : 0u;
                consumed_target = next_target;
                if (next_target != 0) completed_te_scanout = next_target;
                if (interactive_new_frame)
                    interactive_last_presented = interactive_completed;
            } else {
                guest_clt.pump(0xffffffffu, "all");
                // Presentation stays bounded by the application frame the
                // guest has reached on this path too, so a frame the guest has
                // not published from cannot select the scanout. With pacing off
                // no boundary is ever published, so every queued choice is
                // eligible. The queue is drained one unit per iteration here as
                // well, so a multi-frame authored transition is not collapsed.
                const uint32_t available =
                    frame_pacing ? m2_guest_current_frame_c() : 0xffffffffu;
                consumed_boundary = available;
                const uint32_t next_target =
                    guest_clt.consume_display_target_upto(available,
                                                          &consumed_tag);
                consumed_target = next_target;
                if (next_target != 0) completed_te_scanout = next_target;
            }
#else
            guest_clt.pump();
#endif
            guest_clt.service_framebuffer_reads();
            if (frame_timing_enabled)
                pump_ns = frame_timing_now() - timing_pump_start;
#ifdef M2_HAVE_RECOMP
            uint64_t display_generation = 0;
            uint64_t display_content_hash = 0;
            const bool direct_display = m2_guest_display_copy_hashed(
                display_rgba.data(), display_rgba.size(), &display_generation,
                &display_content_hash);
            gfx->set_bitmap_scanout_view(
                direct_display ? display_rgba.data() : nullptr,
                M2_DISPLAY_WIDTH, M2_DISPLAY_HEIGHT, display_content_hash);
            const uint32_t te_attachment = m2_guest_display_te_target();
            // One scanout rule on every pacing path: present the guest's own
            // DISPLAYFRAMEBUFFER choice only once the frame that published it
            // reached a loop head. The runtime's latest View attachment is the
            // bootstrap only, because the guest can advance it from a frame
            // that is still being built; on the non-interactive/diagnostic path
            // every m2ctl command suspends the guest and drains the queue, so
            // that attachment can name an in-flight frame.
            // The direct-bitmap/FMV path is unaffected: every backend gives
            // has_bitmap_scanout() precedence over the TE target in
            // frame_present(), so a latched choice cannot pin the overlay.
            const bool scanout_from_latch = completed_te_scanout != 0;
            const uint32_t te_scanout =
                scanout_from_latch ? completed_te_scanout : te_attachment;
            gfx->clt_scanout_select(te_scanout);
            // Bounded display-carve content dump: sample the target the guest's
            // own DISPLAYFRAMEBUFFER selected, at the moment that display unit
            // is consumed, so a composition that never reached it (unchanged
            // content) can be told apart from one blended over stale content
            // (changed content whose extent is not the authored draw).
            if (consumed_target != 0) {
                CarveDumpSink::instance().maybe_dump(
                    gfx.get(), ran_frames, consumed_target, consumed_tag,
                    consumed_target, consume_mode);
            }
            // Bounded CLT transaction trace (M2_CLT_TRANSACTION_TRACE): one
            // line per presenter iteration tying the guest frames, pump
            // phases, display selection, and scanout together so a partial or
            // early display can be attributed instead of guessed. Observation
            // only; it adds no work when disabled.
            static const bool xact_trace_enabled = [] {
                const char* value = std::getenv("M2_CLT_TRANSACTION_TRACE");
                return value && value[0] != '\0' && value[0] != '0';
            }();
            static const uint32_t xact_trace_from = [] {
                const char* value =
                    std::getenv("M2_CLT_TRANSACTION_TRACE_FROM");
                if (!value || value[0] == '\0') return 0u;
                return static_cast<uint32_t>(std::strtoul(value, nullptr, 0));
            }();
            if (xact_trace_enabled &&
                static_cast<uint32_t>(ran_frames) >= xact_trace_from) {
                static std::atomic<uint32_t> xact_frames_logged{0};
                if (xact_frames_logged.fetch_add(1) < 4000u) {
                    std::fprintf(
                        stderr,
                        "[xact-frame] host=%d completed=%u current=%u "
                        "new=%d completed_work=%d inflight_work=%d "
                        "pumped_completed=%d pumped_inflight=%d "
                        "consumed=%#x consumed_tag=%u bound=%#x mode=%s src=%s "
                        "scanout=%#x attachment=%#x "
                        "direct=%d overlay=%d present=1\n",
                        ran_frames, interactive_completed, interactive_current,
                        interactive_new_frame ? 1 : 0,
                        interactive_has_work ? 1 : 0,
                        interactive_inflight_work ? 1 : 0,
                        pumped_completed ? 1 : 0, pumped_inflight ? 1 : 0,
                        consumed_target, consumed_tag, consumed_boundary,
                        consume_mode,
                        scanout_from_latch ? "latch" : "view", te_scanout,
                        m2_guest_display_te_target(), direct_display ? 1 : 0,
                        video_overlay ? 1 : 0);
                }
            }
#endif
            // Retain the just-rendered target before presentation. The GL
            // backends keep guest-addressed attachments and blit the active
            // target to the window on present; unbound/default-buffer paths
            // still require this pre-swap readback ordering.
            guest_clt.retain_presented_frame();
            ShotSink::instance().maybe_shot(gfx.get(), ran_frames);
            HotkeyShot::instance().poll(gfx.get(), ran_frames);
            if (!cfg.screenshot_path.empty()) {
                if (cfg.screenshot_frame == ran_frames ||
                    (cfg.screenshot_frame < 0 && (quit || (cfg.frames > 0 && ran_frames + 1 >= cfg.frames)))) {
                    const std::filesystem::path requested = cfg.screenshot_path;
                    // Clear before the attempt so a failed explicit request is
                    // reported once instead of retried every later frame.
                    cfg.screenshot_path.clear();
                    if (!save_ppm_screenshot(gfx.get(), requested) &&
                        exit_code == 0) {
                        exit_code = 3;
                    }
                }
            }
            const uint64_t timing_present_start =
                frame_timing_enabled ? frame_timing_now() : 0;
            {
                m2host::perf::Scope renderer_scope(
                    m2host::perf::Phase::RendererWork);
                guest_clt.host_frame_present(true);
                gfx->frame_present();
#ifdef M2_HAVE_RECOMP
                // Name the image the presented pixels actually came from. Every
                // backend's frame_present() gives a direct Bitmap/Projector
                // scanout precedence over the addressed TE target
                // (gl_backend.cpp, clean_renderer.cpp), so acknowledge zero
                // rather than the consumed target when that scanout owned the
                // screen: the runtime then records an explicit ownership
                // refusal instead of reporting the TE unit visible.
                if (consumed_target != 0)
                    m2_guest_te_display_presented(
                        gfx->has_bitmap_scanout() ? 0u : consumed_target);
                m2_guest_display_presented(display_generation);
#endif
            }
            if (frame_timing_enabled) {
                present_ns = frame_timing_now() - timing_present_start;
                const uint64_t total_ns = frame_timing_now() - timing_iter_start;
                (void)total_ns;
#ifdef M2_HAVE_RECOMP
                // 3600 lines keeps a >=10 s steady window assertable even when
                // the display runs at 120 Hz (900 lines is only ~7.5 s there);
                // the end-of-run summary remains the whole-run authority.
                if (frame_timing_logged < 3600u &&
                    paced_frame_before >= frame_timing_floor) {
                    std::fprintf(stderr,
                                 "[frame-timing] host=%d guest=%u wait_ms=%.2f "
                                 "pump_ms=%.2f present_ms=%.2f audio_ms=%.2f "
                                 "iter_ms=%.2f completed=%u\n",
                                 ran_frames, paced_frame_before,
                                 wait_ns / 1e6, pump_ns / 1e6,
                                 present_ns / 1e6, audio_ns / 1e6,
                                 total_ns / 1e6,
                                 m2_guest_frame_pacing_completed());
                    ++frame_timing_logged;
                }
                // Steady-window sampling for the end-of-run summary. Capped at
                // kFrameTimingMaxSamples so a long session cannot grow the
                // vector without bound; once full the summary reports the
                // first window (still >=10 s for any realistic cadence).
                if (paced_frame_before >= frame_timing_floor &&
                    ft_iter_ms.size() < kFrameTimingMaxSamples) {
                    ft_iter_ms.push_back(static_cast<float>(total_ns) / 1e6f);
                    ft_wait_ms.push_back(static_cast<float>(wait_ns) / 1e6f);
                    ft_pump_ms.push_back(static_cast<float>(pump_ns) / 1e6f);
                    ft_present_ms.push_back(static_cast<float>(present_ns) / 1e6f);
                    ft_audio_ms.push_back(static_cast<float>(audio_ns) / 1e6f);
                    if (total_ns > 16667000ull) ++ft_over_16_67;
                    if (ft_have) {
                        // Interactive pacing permits at most one completed
                        // application frame per VBL slot; a larger advance
                        // means the pacing contract slipped.
                        if (paced_frame_before - ft_guest_last > 1u)
                            ++ft_multi_frame;
                    }
                    if (!ft_have) {
                        ft_have = true;
                        ft_first_time = frame_timing_now();
                        ft_guest_first = paced_frame_before;
                    }
                    ft_last_time = frame_timing_now();
                    ft_guest_last = paced_frame_before;
                }
#endif
            }
            // The inactive SDL adapter must not record neutral samples as
            // if they were diagnostic inputs. The controller owns receipts.
            if (!diagnostic.enabled()) sdl_input.end_frame(driving_input);
            ++ran_frames;
            if (diagnostic.enabled()) diagnostic.after_tick();
            if (!cfg.fast_forward && !frame_pacing) {
                if (!cfg.headless) SDL_Delay(16);
                else std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }

        // Explicit single-shot screenshot still pending means the requested
        // frame was never reached, so no output was produced. Report that as
        // a failed request rather than exiting as if the image had been saved.
        if (!cfg.screenshot_path.empty()) {
            std::fprintf(stderr,
                         "[screenshot] requested output %s was not written "
                         "(frame %d not reached)\n",
                         m2host::platform::path_to_utf8(cfg.screenshot_path).c_str(),
                         cfg.screenshot_frame);
            cfg.screenshot_path.clear();
            if (exit_code == 0) exit_code = 3;
        }
        // A requested --capture is explicit output. The software backend keeps
        // the first output error so a create/open/write/close failure cannot be
        // lost inside frame_present().
        if (soft_capture && soft_capture->has_output_error()) {
            std::fprintf(stderr, "[capture] output error: %s\n",
                         soft_capture->output_error().c_str());
            if (exit_code == 0) exit_code = 3;
        }

        // Bounded cadence summary (M2_FRAME_TIMING): menu 60 fps means p95
        // iteration <= 16.67 ms and >=95% of iterations <= 16.67 ms over a
        // >=10 s steady window. This writes the
        // percentiles, the missed-vsync count, and the guest frame period as
        // one line so a saved log can be checked afterwards. Percentiles sort a copy on exit
        // only; nothing here touches pacing.
        if (frame_timing_enabled && !ft_iter_ms.empty()) {
            auto ft_percentile = [](std::vector<float>& v, double p) -> float {
                if (v.empty()) return 0.0f;
                std::sort(v.begin(), v.end());
                size_t idx = static_cast<size_t>(p * (v.size() - 1) + 0.5);
                if (idx >= v.size()) idx = v.size() - 1;
                return v[idx];
            };
            const float it_p50 = ft_percentile(ft_iter_ms, 0.50);
            const float it_p95 = ft_percentile(ft_iter_ms, 0.95);
            const float it_p99 = ft_percentile(ft_iter_ms, 0.99);
            const float it_max = ft_iter_ms.back();
            const float wait_p50 = ft_percentile(ft_wait_ms, 0.50);
            const float pump_p50 = ft_percentile(ft_pump_ms, 0.50);
            const float present_p50 = ft_percentile(ft_present_ms, 0.50);
            const float audio_p50 = ft_percentile(ft_audio_ms, 0.50);
            const double window_s = ft_last_time > ft_first_time
                ? static_cast<double>(ft_last_time - ft_first_time) / 1e9
                : 0.0;
            const uint32_t guest_frames = ft_guest_last > ft_guest_first
                ? ft_guest_last - ft_guest_first : 0u;
            const double guest_fps = window_s > 0.0
                ? static_cast<double>(guest_frames) / window_s : 0.0;
            const double over_pct = 100.0 * static_cast<double>(ft_over_16_67) /
                                    static_cast<double>(ft_iter_ms.size());
            std::fprintf(stderr,
                         "[frame-timing-summary] samples=%zu window_s=%.2f "
                         "p50_ms=%.2f p95_ms=%.2f p99_ms=%.2f max_ms=%.2f "
                         "over_16_67=%u (%.1f%%) multi_frame=%u "
                         "wait_p50_ms=%.2f pump_p50_ms=%.2f "
                         "present_p50_ms=%.2f audio_p50_ms=%.2f "
                         "guest_frames=%u guest_fps=%.2f\n",
                         ft_iter_ms.size(), window_s, it_p50, it_p95, it_p99,
                         it_max, ft_over_16_67, over_pct, ft_multi_frame,
                         wait_p50, pump_p50, present_p50, audio_p50,
                         guest_frames, guest_fps);
            std::fflush(stderr);
        }

        // Final snapshot — guest-owned record, no host fallback.
        // Lenient for short probes (<60 host frames) like ctest's 8-frame smoke;
        // the gate's 12-check leniency is mirrored here. Also drain a grace
        // window so an async task that hasn't yet ticked gets a chance.
#ifdef M2_HAVE_RECOMP
        {
            uint32_t seq = 0, pc = 0, state = 0, snap[4] = {0, 0, 0, 0};
            // Grace drain: keep bridging VBL for up to ~150 ms to let the
            // StateDriver reach its first L2fb88 tick. This covers the race
            // where the host 8-frame pump finishes before the guest thread
            // has been scheduled.
            for (int _grace = 0; _grace < 30; ++_grace) {
                if (m2_guest_frame_query(&seq, &pc, &state, snap, 4) != 0) break;
                if (seq != 0) break;
                // Keep VBL live during grace so a blocking WaitSignal can wake.
                (void)m2_guest_signal_vbl();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            // Re-query for final print after grace
            (void)m2_guest_frame_query(&seq, &pc, &state, snap, 4);
            final_guest_frames = seq;
            std::printf("[guest] final_frame seq=%u pc=0x%x state=%u snap=[%u %u %u %u] host_frames=%d\n",
                        seq, pc, state, snap[0], snap[1], snap[2], snap[3], ran_frames);
            if (seq == 0 && ran_frames >= 60) {
                std::fprintf(stderr,
                             "[guest_gate] FAIL no guest frame produced in %d host frames "
                             "(StateDriver not reaching L2fb88)\n",
                             ran_frames);
                if (exit_code == 0) exit_code = 4;
            } else if (seq != 0) {
                std::printf("[guest] boot boundary proven: seq=%u state=%u pc=0x%x — guest owns frames\n",
                            seq, state, pc);
            } else {
                // Short probe lenient: seq==0 but <60 frames is not a failure
                std::printf("[guest] short probe — seq=0 lenient (need >=60 frames to gate)\n");
            }
            int32_t tstate = -1, tstatus = -1;
            if (m2_guest_main_query(&tstate, &tstatus) == 0) {
                const char* tname = (tstate == 1 ? "RUNNING" : (tstate == 0 ? "STARTED" : (tstate == 2 ? "COMPLETED" : "FAILED")));
                std::printf("[guest] task state=%d (%s) status=%d\n", tstate, tname, tstatus);
                // Parked-task check: a live task that produced no frames fails the run.
                if ((tstate == 0 || tstate == 1) && seq == 0 && exit_code == 0 && ran_frames >= 60) {
                    exit_code = 4;
                }
            }
        }
#endif
        // Persist host-owned settings only from an interactive session. A
        // headless smoke/replay/CI run must be observational and never
        // mutate the user's settings.
        if (!diagnostic.enabled() && !cfg.headless) {
            settings.save();
        }
    }

    guest_clt.dump_z_census();

    // Exact guest-memory fault census, printed *before* the [gfx] summary so a
    // reader (or gate) holding the summary is guaranteed to hold this line too.
    // It is emitted only when the census is armed: the counter is not even
    // incremented otherwise, so an unarmed run must report no number rather
    // than a zero that would read as a clean result.
    if (std::getenv("M2_OOB_COUNT") != nullptr) {
        std::printf("[guest] oob_traps=%llu (uncapped census)\n",
                    static_cast<unsigned long long>(m2_oob_trap_total()));
    }

    // Bounded guest-task lifecycle: query + join only if already terminal;
    // a still-running StateDriver is reported live and guest-kernel teardown
    // is skipped so no live task ever sees destroyed state.
    const m2host::GuestCltStats clt_stats = guest_clt.stats();
    // The addressed-target pool belongs to the renderer, not the CLT bridge, so
    // its bounded-capacity accounting is asked for through the backend
    // interface and printed beside the backend that produced it: a zero from a
    // backend with no pool (software capture, null) is not evidence that the GL
    // pool released nothing. See ICelBlitterBackend::clt_target_pool.
    uint64_t target_releases = 0, target_refusals = 0, target_pool = 0;
    if (gfx) gfx->clt_target_pool(target_releases, target_refusals, target_pool);
    const char* backend_name = gfx ? gfx->name() : "none";
#ifdef M2_HAVE_RECOMP
    const uint64_t te_ack_refusals = m2_guest_te_display_refusals();
#else
    const uint64_t te_ack_refusals = 0;
#endif
    std::printf("[gfx] guest CLT submitted=%llu dropped=%llu queue_full=%llu "
                "malformed=%llu unsupported=%llu draws=%llu "
                "flow_jumps=%llu flow_pauses=%llu flow_external=%llu "
                "flow_refusals=%llu flow_loops=%llu span_evictions=%llu "
                "fb_span_missing=%llu "
                "peak_queue=%llu "
                "peak_bytes=%llu peak_pump=%llu te_ack_refusals=%llu "
                "target_releases=%llu target_refusals=%llu target_pool=%llu "
                "backend=%s\n",
                static_cast<unsigned long long>(clt_stats.submitted),
                static_cast<unsigned long long>(clt_stats.dropped),
                static_cast<unsigned long long>(clt_stats.queue_full_drops),
                static_cast<unsigned long long>(clt_stats.malformed),
                static_cast<unsigned long long>(clt_stats.unsupported),
                static_cast<unsigned long long>(clt_stats.draws),
                static_cast<unsigned long long>(clt_stats.control_flow_jumps),
                static_cast<unsigned long long>(clt_stats.control_flow_pauses),
                static_cast<unsigned long long>(
                    clt_stats.control_flow_external_handoffs),
                static_cast<unsigned long long>(clt_stats.control_flow_refusals),
                static_cast<unsigned long long>(clt_stats.control_flow_loops),
                static_cast<unsigned long long>(clt_stats.span_evictions),
                static_cast<unsigned long long>(clt_stats.fb_span_missing),
                static_cast<unsigned long long>(clt_stats.peak_queued_lists),
                static_cast<unsigned long long>(clt_stats.peak_queued_bytes),
                static_cast<unsigned long long>(clt_stats.peak_pump_batch),
                static_cast<unsigned long long>(te_ack_refusals),
                static_cast<unsigned long long>(target_releases),
                static_cast<unsigned long long>(target_refusals),
                static_cast<unsigned long long>(target_pool),
                backend_name
                );
    guest_clt.detach();
    guest_clt.release_resources();
    if (dspp_worker.joinable()) {
        // Capture analysis excludes the device tail after producer shutdown.
        if (std::getenv("M2_AUDIO_CAPTURE"))
            std::fprintf(stderr, "[audio-capture] active_end=%u\n", audio.rendered_frames());
        dspp_worker_stop.store(true, std::memory_order_release);
        dspp_worker.join();
        m2_guest_audio_set_async_dspp(0);
    }
    // Post-run measured census: the boot report above runs while the guest main
    // task is still starting, so its observed column is empty by construction.
    // This runs after the bounded loop, with the guest still up.
    m2host::folio_print_observed_report(stdout);

    // Savestate save after the pump, before teardown: m2_recomp_shutdown()
    // unbinds the guest kernel and clears its task/item pools, so a save taken
    // after it would record an empty machine.
    if (!cfg.savestate_save.empty()) {
#ifdef M2_HAVE_RECOMP
        auto sr = m2host::savestate::save_to_file(cfg.savestate_save);
        if (!sr.ok) {
            std::fprintf(stderr, "[savestate] save FAILED %s: %s\n", cfg.savestate_save.c_str(), sr.error.c_str());
            if (exit_code == 0) exit_code = 4;
        } else {
            std::printf("[savestate] saved %s hash %016llx\n", cfg.savestate_save.c_str(), (unsigned long long)sr.hash);
        }
#else
        std::fprintf(stderr, "[savestate] save requires M2_HAVE_RECOMP\n");
#endif
    }

    m2_recomp_shutdown();

    m2host::perf::RunSummary perf_summary;
    perf_summary.backend = gfx ? gfx->name() : "unknown";
    perf_summary.host_frames = static_cast<uint64_t>(ran_frames);
    perf_summary.guest_frames = final_guest_frames;
    perf_summary.clt_submitted = clt_stats.submitted;
    perf_summary.clt_dropped = clt_stats.dropped;
    perf_summary.clt_queue_full_drops = clt_stats.queue_full_drops;
    perf_summary.clt_malformed = clt_stats.malformed;
    perf_summary.clt_unsupported = clt_stats.unsupported;
    perf_summary.clt_draws = clt_stats.draws;
    perf_summary.clt_peak_queue = clt_stats.peak_queued_lists;
    perf_summary.clt_peak_bytes = clt_stats.peak_queued_bytes;
    perf_summary.clt_peak_pump = clt_stats.peak_pump_batch;
    perf_summary.headless = cfg.headless;
    perf_summary.paced = frame_pacing;
    perf_summary.fast_forward = cfg.fast_forward;
    if (!m2host::perf::finish(perf_summary) && exit_code == 0)
        exit_code = 5;  // requested --perf-file could not be written

    std::printf("[boot] ran %d host frames — shutting down\n", ran_frames);
    gfx->clear_bitmap_scanout();
    gfx->shutdown();
    gfx.reset();
    if (window) SDL_DestroyWindow(window);
    audio.shutdown();
    broker.shutdown();
    filesystem.shutdown();
    host_controls.close_devices();
    if (!cfg.headless) SDL_Quit();

    // Savestate save after pump
    if (!cfg.savestate_save.empty()) {
#ifdef M2_HAVE_RECOMP
        auto sr = m2host::savestate::save_to_file(cfg.savestate_save);
        if (!sr.ok) {
            std::fprintf(stderr, "[savestate] save FAILED %s: %s\n", cfg.savestate_save.c_str(), sr.error.c_str());
            if (exit_code == 0) exit_code = 4;
        } else {
            std::printf("[savestate] saved %s hash %016llx\n", cfg.savestate_save.c_str(), (unsigned long long)sr.hash);
        }
#else
        std::fprintf(stderr, "[savestate] save requires M2_HAVE_RECOMP\n");
#endif
    }
    // Trace shutdown — emit final shutdown record and flush
    {
        m2host::trace::trace_shutdown();
        if (!cfg.record_input.empty()) m2host::trace::input_recorder().stop_recording();
        if (!cfg.replay_input.empty()) m2host::trace::input_recorder().stop_replay();
        if (!cfg.record_broker.empty()) m2host::trace::input_recorder().stop_broker_recording();
        if (!cfg.replay_broker.empty()) m2host::trace::input_recorder().stop_broker_replay();
    }

    // Smoke-test the selector dispatch path end-to-end.
    auto* probe = reinterpret_cast<int (*)(const char*, ...)>(m2_selector_lookup(M2_FOLIO_KERNEL, 309));
    if (probe) probe("[main] selector dispatch OK: kernel:309 printf reachable (%s)\n", "shims alive");
    void* stub = m2_selector_lookup(M2_FOLIO_AUDIO, 70);
    std::printf("[main] audio:70 resolves to %s\n",
                stub ? "a selector entry (typed status path)" : "null");

    // If any guest task is still executing on its host thread, running C++
    // static destructors would free host services (MPEG codec contexts, audio
    // sinks, trace state) that the live task can still call. The guest kernel
    // is deliberately left intact for the same reason in m2_recomp_shutdown();
    // terminate without host static teardown here so the two decisions agree.
    const int live_tasks = m2_guest_live_task_count();
#ifdef M2_HAVE_RECOMP
    // Bounded kernel-lock attribution dump (M2_KERNEL_LOCK_DIAG); no-op unless
    // the environment enabled the collector. Runs here because the live-task
    // exit path below skips atexit handlers.
    m2_guest_kernel_lock_diag_dump();
    m2_kernel_lock_diag_dump_pairs();
    m2_audio_graph_lock_diag_dump();
#endif
    if (live_tasks > 0) {
        std::fprintf(stderr,
                     "[main] %d guest task(s) still live at exit — skipping host "
                     "static teardown\n", live_tasks);
        std::fflush(nullptr);
        std::_Exit(exit_code);
    }
    std::exit(exit_code);
}
