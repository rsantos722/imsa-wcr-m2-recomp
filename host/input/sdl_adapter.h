// sdl_adapter.h — drains the SDL3 event stream into HostControls and
// publishes the resulting action levels to InputMap.
//
// Only this TU and host_controls.cpp include SDL headers; input_map.{h,cpp}
// stay SDL-free so the level-to-port mapping is testable headless.
#pragma once

#include "input_map.h"
#include "host_controls.h"

namespace m2host {
namespace input {

class SdlInputAdapter {
public:
    // Feeds every SDL event to `controls` and publishes its levels to `map`.
    // During replay, physical input is drained and ignored; window close still
    // works. Returns true when at least one SDL event was consumed.
    bool poll_sdl(InputMap& map, HostControls& controls, bool replay = false);
    bool close_requested() const { return close_; }

    // Frame-end marker: lets InputMap latch per-frame state deterministically.
    void end_frame(InputMap& map) const { map.tick(); }

private:
    bool close_ = false;
};

}  // namespace input
}  // namespace m2host
