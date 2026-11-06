// event_broker.cpp — maps the SDL3 event stream onto a 3DO-style event queue.
#include "event_broker.h"

#include <SDL3/SDL.h>

namespace m2host {

bool EventBroker::init(bool headless) {
    m_headless = headless;
    if (headless) return true;
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            SDL_Log("event_broker: SDL_InitSubSystem(VIDEO) failed: %s", SDL_GetError());
            return false;
        }
    }
    return true;
}

void EventBroker::shutdown() {}

void EventBroker::pump() {
    if (m_headless) return;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        M2EventRecord r{};
        switch (ev.type) {
        case SDL_EVENT_QUIT:
            r.what = M2_EV_QUIT;
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            r.what = M2_EV_KEY;
            r.data = static_cast<uint32_t>(ev.key.key);
            r.x = (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) ? 1 : 0;
            r.y = ev.key.repeat ? 1 : 0;
            break;
        case SDL_EVENT_MOUSE_MOTION:
            r.what = M2_EV_MOUSE_MOTION;
            r.x = static_cast<int32_t>(ev.motion.x);
            r.y = static_cast<int32_t>(ev.motion.y);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            r.what = M2_EV_MOUSE_BUTTON;
            r.x = static_cast<int32_t>(ev.button.x);
            r.y = static_cast<int32_t>(ev.button.y);
            r.data = (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) ? 1u : 0u;
            break;
        default:
            continue;
        }
        int next = (m_write + 1) % kRingSize;
        if (next == m_read) continue;  // drop on overflow
        m_ring[m_write] = r;
        m_write = next;
    }
}

bool EventBroker::poll(M2EventRecord* out) {
    if (m_read == m_write) return false;
    *out = m_ring[m_read];
    m_read = (m_read + 1) % kRingSize;
    return true;
}

void EventBroker::inject(const M2EventRecord& rec) {
    const int next = (m_write + 1) % kRingSize;
    if (next == m_read) return;
    m_ring[m_write] = rec;
    m_write = next;
}

}  // namespace m2host
