// broker_bridge.cpp — implementation. See broker_bridge.h.
//
// Reply-shape evidence (generated game_text_01.c,
// FN_ConnectEventBroker__0f770):
//   0xf918 WaitPort(port,msg) -> r27 = LookupItem(msg)
//   0xf92c lwz r29, 0x28(r27) ; cmpwi 0  -> error path if zero
//   0xf958 lwz r31, 0x2c(r27) ; cmpwi 0  -> error path if zero
//   0xf97c lwz r31, 4(r31)    ; printf(fmt, r31)
// i.e. the message record carries a device count at +0x28 and a pointer to a
// node whose +0x4 is a printable descriptor. Layout PROVISIONAL beyond that.
#include "broker_bridge.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "shim_abi.h"
#include "folio_loader.h"
#include "input_map.h"

namespace m2host {
namespace input {

namespace {

constexpr m2_item_t kItemTag = int32_t(0x62720000);  // 'br..' bridge items
constexpr uint32_t kRecordSize = 0x40;

struct BridgeItem {
    m2_item_t id;
    enum class Kind : uint8_t { MsgPort, BufferedMsg } kind = Kind::MsgPort;
    std::vector<uint8_t> record;  // LookupItem-visible block (kRecordSize)
    m2_item_t bound_port = 0;     // BufferedMsg: owning port
    bool reply_ready = false;     // BufferedMsg: broker answered
};

std::vector<BridgeItem> g_items;
bool g_registered = false;
const ControlPortState* g_control_port = nullptr;
m2_item_t g_broker_item = kItemTag | 1;

// Static descriptor the guest prints after a successful connect.
const char kBrokerDescriptor[] = "M2Host EventBroker v1 (SDL3)";

BridgeItem* find_item(m2_item_t id) {
    if (!id || (id & ~0xFFFF) != kItemTag) return nullptr;
    const size_t idx = size_t(id & 0xFFFF);
    return idx < g_items.size() && g_items[idx].id == id ? &g_items[idx]
                                                         : nullptr;
}

m2_item_t alloc_item(BridgeItem::Kind kind, m2_item_t port) {
    // Id encodes the exact vector slot so find_item is O(1) and total.
    BridgeItem it;
    it.id = kItemTag | m2_item_t(g_items.size());
    it.kind = kind;
    it.bound_port = port;
    it.record.assign(kRecordSize, 0);
    g_items.push_back(std::move(it));
    return g_items.back().id;
}

// Fill the reply fields ConnectEventBroker reads after each WaitPort.
void fill_broker_reply(BridgeItem* msg) {
    if (!msg || msg->record.size() < kRecordSize) return;
    // +0x28: device count. One virtual device (the host remapped pad);
    // RegisterDevices may add more later — bump here when it does.
    static const uint32_t kDeviceCount = 1;
    // +0x2c: pointer to { u32 flags@0; const char* desc@4 } — the guest prints
    // node->desc after connect via `lwz r31, 4(r31)` (0xf97c), i.e. PPC32
    // packed layout with the pointer at +4. Built as raw bytes because a
    // packed host struct would place an 8-byte pointer unaligned (arm64e
    // rejects that); BE-word pinning happens in the input emitter work.
    alignas(8) static uint8_t kNodeBytes[16] = {0};
    if (kNodeBytes[4] == 0 && kNodeBytes[5] == 0) {
        const char* desc = kBrokerDescriptor;
        std::memcpy(kNodeBytes + 4, &desc, sizeof(desc));
    }
    std::memcpy(&msg->record[0x28], &kDeviceCount, 4);
    const uintptr_t node = reinterpret_cast<uintptr_t>(kNodeBytes);
    std::memcpy(&msg->record[0x2c], &node, sizeof(node));
    msg->reply_ready = true;
}

// --- selector handlers (EABI-flavoured C signatures) ---------------------------

m2_item_t m2in_FindNamedItem(int32_t type, const char* name) {
    // controls.c probes type 0x10a with "eventbroker"; anything else falls
    // back to "not found" so unrelated lookups keep their old stub behavior.
    (void)type;
    if (name && std::strcmp(name, "eventbroker") == 0) return g_broker_item;
    return 0;
}

m2_item_t m2in_CreateMsgPort(const char* name, int32_t prio, int32_t arg) {
    (void)prio;
    (void)arg;
    std::printf("[broker_bridge] CreateMsgPort(\"%s\")\n", name ? name : "");
    return alloc_item(BridgeItem::Kind::MsgPort, 0);
}

m2_item_t m2in_CreateBufferedMsg(int32_t a, int32_t b, m2_item_t port,
                                 uint32_t bufsize) {
    (void)a;
    (void)b;
    (void)bufsize;  // guest uses 0x100; our records are fixed-size
    return alloc_item(BridgeItem::Kind::BufferedMsg, port);
}

int32_t m2in_SendMsg(m2_item_t dest, m2_item_t msg_item, void* buf,
                     uint32_t len) {
    BridgeItem* m = find_item(msg_item);
    if (!m) return -1;
    if (buf && len) {
        const uint32_t n = len < kRecordSize ? len : kRecordSize;
        std::memcpy(m->record.data(), buf, n);
    }
    if (dest == g_broker_item) {
        // Broker answers synchronously: registration and query messages both
        // get the device-count/descriptor reply before the next WaitPort.
        fill_broker_reply(m);
        if (g_control_port) {
            // Mirror live input into the tail of the buffer so guest-side
            // polling code can see movement without extra vectors
            // (PROVISIONAL until Joy_TranslateInput lift pins the protocol).
            // Tail only: +0x38.. must NOT overlap the +0x2c node pointer.
            const ControlPortState& p = *g_control_port;
            if (m->record.size() >= 0x40) {
                std::memcpy(&m->record[0x38], &p.buttons, 4);
                m->record[0x3c] = p.stick_x;
                m->record[0x3d] = p.stick_y;
                m->record[0x3e] = p.device_index;
            }
        }
        return M2_OK;
    }
    return M2_OK;  // port-to-port mail: stored, single-threaded model
}

m2_item_t m2in_WaitPort(m2_item_t port, m2_item_t msg_item) {
    (void)port;
    // Host never blocks: SendMsg already prepared the reply.
    BridgeItem* m = find_item(msg_item);
    if (m && !m->reply_ready) fill_broker_reply(m);
    return msg_item;
}

// Registry-aware LookupItem override. The guest calls it in single-argument
// item-number form (r3 = CreateBufferedMsg result, FN_ConnectEventBroker
// 0xf81c) and DEREFERENCES the result (0xf92c/0xf958 read +0x28/+0x2c), so
// bridge items must resolve to real host records; everything else keeps the
// kernel_shim pseudo-item behavior ('KRN1').
void* m2in_LookupItem(int32_t item, const char* /*ignored r4*/) {
    if ((item & ~0xFFFF) == kItemTag) {
        if (BridgeItem* it = find_item(item)) return it->record.data();
    }
    return reinterpret_cast<void*>(0x4B524E31 /*'KRN1'*/);
}

}  // namespace

void input_shim_register() {
    if (g_registered) return;
    g_registered = true;
    folio_register(M2_FOLIO_KERNEL, 176,
                   reinterpret_cast<void*>(&m2in_FindNamedItem));
    folio_register(M2_FOLIO_KERNEL, 172,
                   reinterpret_cast<void*>(&m2in_CreateMsgPort));
    folio_register(M2_FOLIO_KERNEL, 168,
                   reinterpret_cast<void*>(&m2in_CreateBufferedMsg));
    folio_register(M2_FOLIO_KERNEL, 150,
                   reinterpret_cast<void*>(&m2in_SendMsg));
    folio_register(M2_FOLIO_KERNEL, 130,
                   reinterpret_cast<void*>(&m2in_WaitPort));
    // Overrides kernel_shim's pseudo LookupItem so guest item derefs work.
    folio_register(M2_FOLIO_KERNEL, 34,
                   reinterpret_cast<void*>(&m2in_LookupItem));
}

void broker_bridge_set_control_port(const ControlPortState* st) {
    g_control_port = st;
}

bool broker_bridge_ready() { return g_registered; }
m2_item_t broker_bridge_broker_item() { return g_broker_item; }

}  // namespace input
}  // namespace m2host
