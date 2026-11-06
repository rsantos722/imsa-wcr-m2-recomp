#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint64_t submitted;
    uint64_t consumed;
    uint64_t completed; // tap includes a consumed release sample
    uint32_t buttons;
    uint32_t phase; // 0 idle, 1 awaiting sample, 2 awaiting tap release
} m2_diagnostic_input_status;
// Host calls only with all guest tasks suspended. Ordinary ControlPort input;
// no guest menu/state mutation. Zero means unavailable or already pending.
uint64_t m2_diagnostic_input_submit(uint32_t buttons, uint8_t x, uint8_t y, int tap);
// Republish the pending level at the normal per-tick platform-input boundary.
// This keeps a pre-initialization request live if EventBroker setup replaces
// its backing buffer before the guest performs the acknowledged copy.
void m2_diagnostic_input_publish(void);
void m2_diagnostic_input_query(m2_diagnostic_input_status* out);
void m2_diagnostic_input_cancel(void);
#ifdef __cplusplus
}
#endif
