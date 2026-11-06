// event_broker.h — kernel event broker shim backed by SDL events.
#pragma once

#include <cstdint>

struct SDL_Window;

namespace m2host {

// M2/3DO-style EventRecord.
struct M2EventRecord {
    uint16_t what;
    int32_t x;
    int32_t y;
    uint32_t data;  // key code / button mask / pad bits
};

enum {
    M2_EV_NONE = 0,
    M2_EV_QUIT = 1,
    M2_EV_KEY = 2,          // data = SDL keycode; x=1 on fresh down edge, y=1 for OS key-repeat
    M2_EV_MOUSE_MOTION = 3, // x,y = position
    M2_EV_MOUSE_BUTTON = 4, // x,y + data bit0=left down etc.
};

class EventBroker {
public:
    // headless=true skips SDL video/event subsystem entirely.
    bool init(bool headless);
    void shutdown();

    // Pump SDL events into the internal ring. No-op in headless mode.
    void pump();
    bool poll(M2EventRecord* out);

    // Push a synthetic record directly into the ring.
    // Works in headless mode too — this is how input remapping and tests feed
    // discrete events (menu keys, pause, quit) without an SDL event stream.
    void inject(const M2EventRecord& rec);

private:
    bool m_headless = false;
    static constexpr int kRingSize = 256;
    M2EventRecord m_ring[kRingSize] = {};
    int m_read = 0;
    int m_write = 0;
};

}  // namespace m2host
