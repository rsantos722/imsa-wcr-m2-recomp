// broker_bridge.h — kernel message-port surface behind CONTROLS.c.
//
// controls.c talks to the Opera **event broker** entirely through kernel-folio
// message-port selectors (the SendEBMessage/RegisterDevices/ConnectEventBroker
// call sites):
//
//   FindNamedItem(176, "eventbroker") -> CreateMsgPort(172) ->
//   CreateBufferedMsg(168) -> LookupItem(34) -> SendMsg(150)/WaitPort(130)
//
// This module implements exactly that surface against host state:
//   * "eventbroker" resolves to a synthetic named item,
//   * SendMsg into the broker synthesizes the registration/query reply the
//     guest's ConnectEventBroker expects (device-count @+0x28, device-list
//     pointer @+0x2c of the LookupItem record — offsets read straight off the
//     FN_ConnectEventBroker__0f770 lift),
//   * WaitPort completes immediately (single-threaded host frame pump).
//
// The continuous control-port snapshot itself lives in InputMap
// (input_map.h); race/game-flow code bridges the two via
// broker_bridge_set_control_port(). Registered into the folio dispatch table
// by input_shim_register() — call it once after folio_install_all().
#pragma once

#include "shim_types.h"

namespace m2host {
namespace input {
struct ControlPortState;
}

namespace input {

// Install this slice's kernel-vector handlers (176/172/168/150/130 plus a
// registry-aware LookupItem override for 34). Safe to call repeatedly; every
// call after the first is a no-op. Must run AFTER folio_install_all().
void input_shim_register();

// Point the broker replies at the live control-port snapshot (ownership stays
// with the caller — typically InputMap::control_port(0)). May be called any
// time after registration; pass nullptr to detach.
void broker_bridge_set_control_port(const ControlPortState* st);

// Test/debug introspection.
bool broker_bridge_ready();
m2_item_t broker_bridge_broker_item();

}  // namespace input
}  // namespace m2host
