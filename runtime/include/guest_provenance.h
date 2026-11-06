#ifndef GUEST_PROVENANCE_H
#define GUEST_PROVENANCE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "guest_events.h"

#ifdef __cplusplus
extern "C" {
#endif

#define M2_PROV_MAX_WATCHED_RANGES 32
#define M2_PROV_MAX_WATCHED_ENTRIES 2048
#define M2_PROV_MAX_ALLOC_RECORDS 512
#define M2_PROV_MAX_HISTORY 1024
#define M2_PROV_MAX_OPERATION_WORDS 4096

typedef struct {
    uint32_t start_ea;
    uint32_t end_ea; // inclusive
    char label[32];
} M2ProvWatchedRange;

typedef struct {
    uint32_t ea;            // 4-byte aligned EA (or exact byte EA)
    uint32_t latest_value;  // Current/latest 32-bit word value
    uint32_t writer_pc;     // PC of the store
    uint32_t caller_lr;     // LR of the caller
    uint32_t frame_seq;     // Guest frame sequence counter
    uint32_t write_count;   // Number of writes recorded
    uint32_t src_ea;        // Source EA if known copy (0 if direct write)
    uint32_t copy_pc;       // PC of copy operation
    uint8_t  last_width;    // 1, 2, 4
    uint32_t last_size;     // Number of bytes in the last operation
    uint32_t src_writer_pc; // PC of the source word's last write, if known
    uint32_t src_caller_lr;
    uint32_t src_frame_seq;
    char     alloc_tag[32]; // e.g. "BSS scratch", "Heap Carve", ".data", etc.
} M2ProvEntry;

typedef struct {
    uint32_t ea;
    uint32_t value;
    uint32_t pc;
    uint32_t lr;
    uint32_t frame_seq;
    uint8_t  width;
    uint8_t  op_type;       // 0=write, 1=copy, 2=alloc, 3=free, 4=read
    uint32_t src_ea;
} M2ProvEvent;

typedef struct {
    uint32_t start_ea;
    uint32_t end_ea;
    uint32_t size;
    uint32_t alloc_pc;
    uint32_t frame_seq;
    uint32_t alloc_id;
    char     tag[32];
    bool     active;
} M2ProvAllocRecord;

typedef struct {
    bool enabled;
    bool log_writes;
    bool log_reads;
    bool log_copies;
    bool log_allocs;
    bool auto_oob_dump;

    size_t num_watched_ranges;
    M2ProvWatchedRange watched_ranges[M2_PROV_MAX_WATCHED_RANGES];

    size_t num_entries;
    M2ProvEntry entries[M2_PROV_MAX_WATCHED_ENTRIES];

    size_t history_head;
    size_t history_count;
    M2ProvEvent history[M2_PROV_MAX_HISTORY];

    size_t num_allocs;
    M2ProvAllocRecord allocs[M2_PROV_MAX_ALLOC_RECORDS];

    char output_json_path[256];
} M2ProvenanceState;

// Fast global check flag (0 when disabled)
extern bool m2_provenance_active;
// Optional read capture is deliberately separate from the active flag: a
// watched write investigation should not turn every guest load into an event.
extern bool m2_provenance_reads_active;

// Lifecycle
void m2_provenance_init(void);
void m2_provenance_flush(void);
void m2_provenance_shutdown(void);
void m2_provenance_reset(void);

// Configuration
bool m2_provenance_add_watched_range(uint32_t start_ea, uint32_t end_ea, const char* label);
bool m2_provenance_is_watched(uint32_t ea, uint8_t width);

// Write / Read / Copy / Alloc recording hooks
void m2_provenance_record_write(uint32_t ea, uint32_t value, uint8_t width, uint32_t pc, uint32_t lr, uint32_t frame_seq);
void m2_provenance_record_read(uint32_t ea, uint32_t value, uint8_t width, uint32_t pc, uint32_t lr, uint32_t frame_seq);
void m2_provenance_record_copy(uint32_t dst_ea, uint32_t src_ea, uint32_t size, uint32_t pc, uint32_t lr, uint32_t frame_seq);
void m2_provenance_record_host_write(const uint8_t* guest_mem, uint32_t ea, uint32_t size,
                                     uint32_t pc, uint32_t lr, uint32_t frame_seq);
void m2_provenance_record_copy_from_guest(const uint8_t* guest_mem, uint32_t dst_ea,
                                          uint32_t src_ea, uint32_t size, uint32_t pc,
                                          uint32_t lr, uint32_t frame_seq);
void m2_provenance_record_alloc(uint32_t start_ea, uint32_t size, uint32_t pc, const char* tag, uint32_t frame_seq);
void m2_provenance_record_free(uint32_t start_ea, uint32_t size, uint32_t pc, uint32_t frame_seq);

// Query / Classification
const char* m2_provenance_classify_address(uint32_t ea, M2ProvAllocRecord* out_alloc);
const M2ProvEntry* m2_provenance_find_entry(uint32_t ea);

// OOB Correlation & Output
void m2_provenance_report_oob(uint32_t oob_ea, uint8_t width, uint32_t pc, uint32_t lr);
bool m2_provenance_dump_json(const char* filepath);

#ifdef __cplusplus
}
#endif

#endif // GUEST_PROVENANCE_H
