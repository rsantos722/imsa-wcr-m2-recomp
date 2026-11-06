// sdl_adapter.cpp — SDL3 input adapter implementation. See sdl_adapter.h.
#include "sdl_adapter.h"

#include <SDL3/SDL.h>

namespace m2host {
namespace input {

bool SdlInputAdapter::poll_sdl(InputMap& map, HostControls& controls, bool replay) {
    bool consumed = false;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        consumed = true;
        if (ev.type == SDL_EVENT_QUIT) { close_ = true; continue; }
        // Window controls remain usable during playback; physical input never
        // reaches the control port or broker. Replay scheduling is unchanged.
        if (replay) continue;
        // Publish after every event so a press and release within one poll
        // still leave their edges on the port.
        controls.event(ev);
        map.set_live_levels(controls.levels());
    }
    if (!replay) {
        map.set_live_levels(controls.levels());
        controls.end_frame();
    }
    return consumed;
}

}  // namespace input
}  // namespace m2host
