#include "guest_provenance.h"
#include "ppc_ctx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#define m2_mkdir(p) _mkdir(p)
#else
#define m2_mkdir(p) mkdir(p, 0755)
#endif

bool m2_provenance_active = false;
bool m2_provenance_reads_active = false;
static M2ProvenanceState s_prov_state;
static pthread_mutex_t s_prov_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t s_prov_oob_reports;

extern unsigned g_recomp_text_size;
extern unsigned g_recomp_data_size;
extern unsigned g_recomp_bss_size;
extern unsigned g_recomp_text_base;
extern unsigned g_recomp_data_base;
extern unsigned g_recomp_bss_base;

typedef struct {
    uint32_t start_ea;
    uint32_t end_ea;
} M2ProvInterval;

static void copy_text(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) return;
    if (!src) src = "";
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static void emit_guest_event(const M2GuestEvent* event) {
    if (event && m2_guest_event_c) m2_guest_event_c(event);
}

static void init_provenance_event(M2GuestEvent* event, uint32_t kind,
                                  uint32_t ea, uint32_t src_ea, uint32_t size,
                                  uint32_t value, uint32_t width, uint32_t pc,
                                  uint32_t lr, uint32_t frame_seq,
                                  const char* tag, const char* region) {
    memset(event, 0, sizeof(*event));
    event->schema_version = M2_GUEST_EVENT_SCHEMA_VERSION;
    event->kind = kind;
    event->pc = pc;
    event->lr = lr;
    event->frame_seq = frame_seq;
    event->ea = ea;
    event->src_ea = src_ea;
    event->size = size;
    event->value = value;
    event->width = width;
    copy_text(event->tag, sizeof(event->tag), tag);
    copy_text(event->region, sizeof(event->region), region);
}

static uint64_t inclusive_end(uint32_t start_ea, uint32_t size) {
    if (size == 0) return start_ea;
    uint64_t end_ea = (uint64_t)start_ea + (uint64_t)size - 1u;
    return end_ea > UINT32_MAX ? UINT32_MAX : end_ea;
}

static void ensure_parent_dir(const char* filepath) {
    char dir[256];
    copy_text(dir, sizeof(dir), filepath);
    char* last_slash = strrchr(dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        if (*dir) m2_mkdir(dir);
    }
}

static void register_default_allocations(void) {
    // Use generated image metadata rather than duplicating section boundaries
    // here. That keeps provenance correct when the recovered image changes.
    m2_provenance_record_alloc(g_recomp_text_base, g_recomp_text_size, 0u,
                               ".text (executable)", 0);
    m2_provenance_record_alloc(g_recomp_data_base, g_recomp_data_size, 0u,
                               ".data (initialized)", 0);
    m2_provenance_record_alloc(g_recomp_bss_base, g_recomp_bss_size, 0u,
                               ".bss (uninitialized)", 0);
    m2_provenance_record_alloc(0x00000000u, 0x00004000u, 0u,
                               "Low Memory (vectors/globals)", 0);
}

static uint32_t parse_hex_or_dec(const char* s, const char** out_end) {
    while (*s && (isspace((unsigned char)*s) || *s == ',')) s++;
    char* end = NULL;
    uint32_t val = (uint32_t)strtoul(s, &end, 0);
    if (out_end) *out_end = end;
    return val;
}

static void parse_ranges_env(const char* env_val) {
    if (!env_val || !*env_val) return;
    if (strcmp(env_val, "1") == 0 || strcmp(env_val, "all") == 0 || strcmp(env_val, "ALL") == 0) {
        // Wildcard mode: track any writes up to entry limit
        s_prov_state.num_watched_ranges = 0;
        return;
    }
    const char* p = env_val;
    while (*p && s_prov_state.num_watched_ranges < M2_PROV_MAX_WATCHED_RANGES) {
        while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == ';')) p++;
        if (!*p) break;
        const char* end = NULL;
        uint32_t start = parse_hex_or_dec(p, &end);
        if (end == p) break;
        p = end;
        uint32_t end_ea = (uint32_t)inclusive_end(start, 4u); // default word
        if (*p == ':' || *p == '-') {
            p++;
            end_ea = parse_hex_or_dec(p, &end);
            p = end;
        }
        char label[32];
        snprintf(label, sizeof(label), "range_%zu", s_prov_state.num_watched_ranges);
        m2_provenance_add_watched_range(start, end_ea, label);
    }
}

static bool is_watched_unlocked(uint32_t ea, uint32_t size) {
    if (!m2_provenance_active || size == 0) return false;
    uint64_t end_ea = inclusive_end(ea, size);
    if (s_prov_state.num_watched_ranges == 0) return true;
    for (size_t i = 0; i < s_prov_state.num_watched_ranges; i++) {
        const M2ProvWatchedRange* range = &s_prov_state.watched_ranges[i];
        if ((uint64_t)range->start_ea <= end_ea &&
            (uint64_t)range->end_ea >= ea) {
            return true;
        }
    }
    return false;
}

static size_t collect_watched_intervals_unlocked(uint32_t ea, uint32_t size,
                                                 M2ProvInterval* intervals) {
    if (size == 0) return 0;
    uint32_t operation_end = (uint32_t)inclusive_end(ea, size);
    size_t count = 0;

    if (s_prov_state.num_watched_ranges == 0) {
        intervals[0].start_ea = ea;
        intervals[0].end_ea = operation_end;
        return 1;
    }

    for (size_t i = 0; i < s_prov_state.num_watched_ranges &&
                       count < M2_PROV_MAX_WATCHED_RANGES; i++) {
        const M2ProvWatchedRange* range = &s_prov_state.watched_ranges[i];
        uint32_t start_ea = ea > range->start_ea ? ea : range->start_ea;
        uint32_t end_ea = operation_end < range->end_ea ? operation_end : range->end_ea;
        if (start_ea <= end_ea) {
            intervals[count].start_ea = start_ea;
            intervals[count].end_ea = end_ea;
            count++;
        }
    }

    // Sort and merge overlaps so a large copy is scanned once per watched
    // word, not once per configured range.
    for (size_t i = 1; i < count; i++) {
        M2ProvInterval current = intervals[i];
        size_t j = i;
        while (j > 0 && intervals[j - 1].start_ea > current.start_ea) {
            intervals[j] = intervals[j - 1];
            j--;
        }
        intervals[j] = current;
    }

    size_t merged_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (merged_count == 0 ||
            (uint64_t)intervals[i].start_ea >
                (uint64_t)intervals[merged_count - 1].end_ea + 1u) {
            intervals[merged_count++] = intervals[i];
        } else if (intervals[i].end_ea > intervals[merged_count - 1].end_ea) {
            intervals[merged_count - 1].end_ea = intervals[i].end_ea;
        }
    }
    return merged_count;
}

void m2_provenance_init(void) {
    m2_provenance_active = false;
    pthread_mutex_lock(&s_prov_lock);
    memset(&s_prov_state, 0, sizeof(s_prov_state));
    s_prov_oob_reports = 0;
    s_prov_state.log_writes = true;
    s_prov_state.log_reads = false;
    s_prov_state.log_copies = true;
    s_prov_state.log_allocs = true;
    s_prov_state.auto_oob_dump = true;
    copy_text(s_prov_state.output_json_path,
              sizeof(s_prov_state.output_json_path),
              "provenance.json");

    const char* env_prov = getenv("M2_PROVENANCE");
    const char* env_out = getenv("M2_PROVENANCE_OUT");
    const char* env_writes = getenv("M2_PROVENANCE_WRITES");
    const char* env_reads = getenv("M2_PROVENANCE_READS");
    const char* env_copies = getenv("M2_PROVENANCE_COPIES");
    const char* env_allocs = getenv("M2_PROVENANCE_ALLOCS");

    if (env_out && *env_out) {
        copy_text(s_prov_state.output_json_path,
                  sizeof(s_prov_state.output_json_path), env_out);
    }
    if (env_writes) s_prov_state.log_writes = (atoi(env_writes) != 0);
    if (env_reads) s_prov_state.log_reads = (atoi(env_reads) != 0);
    if (env_copies) s_prov_state.log_copies = (atoi(env_copies) != 0);
    if (env_allocs) s_prov_state.log_allocs = (atoi(env_allocs) != 0);

    if (env_prov && *env_prov) {
        s_prov_state.enabled = true;
        m2_provenance_active = true;
        m2_provenance_reads_active = s_prov_state.log_reads;
        pthread_mutex_unlock(&s_prov_lock);
        parse_ranges_env(env_prov);
        if (s_prov_state.log_allocs) register_default_allocations();
    } else {
        s_prov_state.enabled = false;
        m2_provenance_reads_active = false;
        pthread_mutex_unlock(&s_prov_lock);
    }
}

void m2_provenance_flush(void) {
    char output_path[sizeof(s_prov_state.output_json_path)];
    pthread_mutex_lock(&s_prov_lock);
    bool should_dump = s_prov_state.enabled &&
                       s_prov_state.output_json_path[0] != '\0';
    copy_text(output_path, sizeof(output_path), s_prov_state.output_json_path);
    pthread_mutex_unlock(&s_prov_lock);
    if (should_dump) (void)m2_provenance_dump_json(output_path);
}

void m2_provenance_shutdown(void) {
    m2_provenance_flush();
    m2_provenance_active = false;
    m2_provenance_reads_active = false;
}

void m2_provenance_reset(void) {
    pthread_mutex_lock(&s_prov_lock);
    s_prov_state.num_watched_ranges = 0;
    s_prov_state.num_entries = 0;
    s_prov_state.history_head = 0;
    s_prov_state.history_count = 0;
    s_prov_state.num_allocs = 0;
    s_prov_oob_reports = 0;
    bool register_defaults = m2_provenance_active && s_prov_state.enabled &&
                             s_prov_state.log_allocs;
    pthread_mutex_unlock(&s_prov_lock);
    if (register_defaults) register_default_allocations();
}

bool m2_provenance_add_watched_range(uint32_t start_ea, uint32_t end_ea, const char* label) {
    pthread_mutex_lock(&s_prov_lock);
    if (s_prov_state.num_watched_ranges >= M2_PROV_MAX_WATCHED_RANGES) {
        pthread_mutex_unlock(&s_prov_lock);
        return false;
    }
    M2ProvWatchedRange* r =
        &s_prov_state.watched_ranges[s_prov_state.num_watched_ranges++];
    r->start_ea = start_ea;
    r->end_ea = (end_ea >= start_ea) ? end_ea : start_ea;
    if (label) {
        copy_text(r->label, sizeof(r->label), label);
    } else {
        snprintf(r->label, sizeof(r->label), "range_%#x", start_ea);
    }
    s_prov_state.enabled = true;
    m2_provenance_active = true;
    pthread_mutex_unlock(&s_prov_lock);
    return true;
}

bool m2_provenance_is_watched(uint32_t ea, uint8_t width) {
    pthread_mutex_lock(&s_prov_lock);
    bool watched = is_watched_unlocked(ea, width);
    pthread_mutex_unlock(&s_prov_lock);
    return watched;
}

static const char* image_region_for_address(uint32_t ea,
                                             M2ProvAllocRecord* out_alloc) {
    if (g_recomp_text_size && ea >= g_recomp_text_base &&
        (uint64_t)ea < (uint64_t)g_recomp_text_base + g_recomp_text_size) {
        if (out_alloc) {
            memset(out_alloc, 0, sizeof(*out_alloc));
            out_alloc->start_ea = g_recomp_text_base;
            out_alloc->size = g_recomp_text_size;
            out_alloc->end_ea = (uint32_t)inclusive_end(g_recomp_text_base,
                                                        g_recomp_text_size);
            out_alloc->active = true;
            copy_text(out_alloc->tag, sizeof(out_alloc->tag),
                      ".text (executable)");
        }
        return ".text (executable)";
    }
    if (g_recomp_data_size && ea >= g_recomp_data_base &&
        (uint64_t)ea < (uint64_t)g_recomp_data_base + g_recomp_data_size) {
        if (out_alloc) {
            memset(out_alloc, 0, sizeof(*out_alloc));
            out_alloc->start_ea = g_recomp_data_base;
            out_alloc->size = g_recomp_data_size;
            out_alloc->end_ea = (uint32_t)inclusive_end(g_recomp_data_base,
                                                        g_recomp_data_size);
            out_alloc->active = true;
            copy_text(out_alloc->tag, sizeof(out_alloc->tag),
                      ".data (initialized)");
        }
        return ".data (initialized)";
    }
    if (g_recomp_bss_size && ea >= g_recomp_bss_base &&
        (uint64_t)ea < (uint64_t)g_recomp_bss_base + g_recomp_bss_size) {
        if (out_alloc) {
            memset(out_alloc, 0, sizeof(*out_alloc));
            out_alloc->start_ea = g_recomp_bss_base;
            out_alloc->size = g_recomp_bss_size;
            out_alloc->end_ea = (uint32_t)inclusive_end(g_recomp_bss_base,
                                                        g_recomp_bss_size);
            out_alloc->active = true;
            copy_text(out_alloc->tag, sizeof(out_alloc->tag),
                      ".bss (uninitialized)");
        }
        return ".bss (uninitialized)";
    }
    if (ea < 0x00004000u) {
        if (out_alloc) {
            memset(out_alloc, 0, sizeof(*out_alloc));
            out_alloc->start_ea = 0;
            out_alloc->size = 0x00004000u;
            out_alloc->end_ea = 0x00003fffu;
            out_alloc->active = true;
            copy_text(out_alloc->tag, sizeof(out_alloc->tag),
                      "Low Memory (vectors/globals)");
        }
        return "Low Memory (vectors/globals)";
    }
    return NULL;
}

static const char* classify_address_unlocked(uint32_t ea,
                                              M2ProvAllocRecord* out_alloc) {
    if (out_alloc) memset(out_alloc, 0, sizeof(*out_alloc));
    for (size_t i = s_prov_state.num_allocs; i > 0; i--) {
        const M2ProvAllocRecord* a = &s_prov_state.allocs[i - 1];
        if (a->active && ea >= a->start_ea && ea <= a->end_ea) {
            if (out_alloc) *out_alloc = *a;
            return a->tag;
        }
    }

    const char* image_tag = image_region_for_address(ea, out_alloc);
    if (image_tag) return image_tag;
    if (ea >= RECOMP_M2_RAM_LIMIT && ea < RECOMP_GUEST_MEM) {
        return "Host-expanded (outside original M2 RAM)";
    }
    if (ea >= 0x00400000u && ea < RECOMP_M2_RAM_LIMIT) {
        return "Guest Heap (untracked carve)";
    }
    if (ea >= RECOMP_GUEST_MEM) return "Out-of-Bounds (>= 256MB)";
    return "Unknown Memory Region";
}

const char* m2_provenance_classify_address(uint32_t ea, M2ProvAllocRecord* out_alloc) {
    pthread_mutex_lock(&s_prov_lock);
    const char* tag = classify_address_unlocked(ea, out_alloc);
    pthread_mutex_unlock(&s_prov_lock);
    return tag;
}

static const M2ProvEntry* find_entry_unlocked(uint32_t ea) {
    uint32_t word_ea = ea & ~3u;
    for (size_t i = 0; i < s_prov_state.num_entries; i++) {
        if (s_prov_state.entries[i].ea == word_ea) {
            return &s_prov_state.entries[i];
        }
    }
    return NULL;
}

const M2ProvEntry* m2_provenance_find_entry(uint32_t ea) {
    pthread_mutex_lock(&s_prov_lock);
    const M2ProvEntry* entry = find_entry_unlocked(ea);
    pthread_mutex_unlock(&s_prov_lock);
    return entry;
}

static M2ProvEntry* get_or_create_entry_unlocked(uint32_t ea) {
    uint32_t word_ea = ea & ~3u;
    for (size_t i = 0; i < s_prov_state.num_entries; i++) {
        if (s_prov_state.entries[i].ea == word_ea) {
            return &s_prov_state.entries[i];
        }
    }
    if (s_prov_state.num_entries < M2_PROV_MAX_WATCHED_ENTRIES) {
        M2ProvEntry* e = &s_prov_state.entries[s_prov_state.num_entries++];
        memset(e, 0, sizeof(*e));
        e->ea = word_ea;
        const char* tag = classify_address_unlocked(word_ea, NULL);
        copy_text(e->alloc_tag, sizeof(e->alloc_tag), tag);
        return e;
    }
    return NULL;
}

static void append_history_event(uint32_t ea, uint32_t value, uint8_t width, uint8_t op_type, uint32_t pc, uint32_t lr, uint32_t frame_seq, uint32_t src_ea) {
    size_t idx = s_prov_state.history_head;
    M2ProvEvent* ev = &s_prov_state.history[idx];
    ev->ea = ea;
    ev->value = value;
    ev->pc = pc;
    ev->lr = lr;
    ev->frame_seq = frame_seq;
    ev->width = width;
    ev->op_type = op_type;
    ev->src_ea = src_ea;

    s_prov_state.history_head = (s_prov_state.history_head + 1) % M2_PROV_MAX_HISTORY;
    if (s_prov_state.history_count < M2_PROV_MAX_HISTORY) {
        s_prov_state.history_count++;
    }
}

static uint32_t read_guest_word(const uint8_t* guest_mem, uint32_t ea) {
    if (!guest_mem) return 0;
    uint32_t value = 0;
    for (uint32_t i = 0; i < 4; i++) {
        uint64_t address = (uint64_t)ea + i;
        value <<= 8;
        if (address < RECOMP_GUEST_MEM) value |= guest_mem[address];
    }
    return value;
}

static uint32_t record_memory_operation_unlocked(const uint8_t* guest_mem,
                                                 bool is_copy, uint32_t dst_ea,
                                                 uint32_t src_ea, uint32_t size,
                                                 uint32_t pc, uint32_t lr,
                                                 uint32_t frame_seq) {
    M2ProvInterval intervals[M2_PROV_MAX_WATCHED_RANGES];
    size_t interval_count =
        collect_watched_intervals_unlocked(dst_ea, size, intervals);
    uint32_t first_value = 0;
    uint32_t first_src_ea = 0;
    uint32_t processed_words = 0;
    uint64_t operation_end = inclusive_end(dst_ea, size);

    for (size_t i = 0; i < interval_count; i++) {
        uint32_t word_start = intervals[i].start_ea & ~3u;
        uint32_t word_end = intervals[i].end_ea & ~3u;
        for (uint64_t word = word_start;
             word <= word_end && processed_words < M2_PROV_MAX_OPERATION_WORDS;
             word += 4u) {
            uint64_t overlap_start = word > dst_ea ? word : dst_ea;
            uint64_t overlap_end = word + 3u < operation_end
                                       ? word + 3u
                                       : operation_end;
            if (overlap_start > overlap_end) continue;

            uint32_t word_ea = (uint32_t)word;
            uint32_t value = read_guest_word(guest_mem, word_ea);
            const M2ProvEntry* destination_entry =
                find_entry_unlocked(word_ea);
            uint32_t source_ea = 0;
            const M2ProvEntry* source_entry = NULL;
            if (is_copy) {
                uint64_t source_at_overlap =
                    (uint64_t)src_ea + (overlap_start - dst_ea);
                source_ea = (uint32_t)source_at_overlap;
                source_entry = find_entry_unlocked(source_ea);
                if (!guest_mem && source_entry) value = source_entry->latest_value;
            }
            if (!guest_mem && !source_entry && destination_entry)
                value = destination_entry->latest_value;

            M2ProvEntry* entry = get_or_create_entry_unlocked(word_ea);
            if (entry) {
                entry->latest_value = value;
                entry->writer_pc = pc;
                entry->caller_lr = lr;
                entry->frame_seq = frame_seq;
                entry->write_count++;
                entry->last_width =
                    (uint8_t)(overlap_end - overlap_start + 1u);
                entry->last_size = (uint32_t)(overlap_end - overlap_start + 1u);
                if (is_copy) {
                    entry->src_ea = source_ea;
                    entry->copy_pc = pc;
                    entry->src_writer_pc = source_entry ? source_entry->writer_pc : 0;
                    entry->src_caller_lr = source_entry ? source_entry->caller_lr : 0;
                    entry->src_frame_seq = source_entry ? source_entry->frame_seq : 0;
                } else {
                    entry->src_ea = 0;
                    entry->copy_pc = 0;
                    entry->src_writer_pc = 0;
                    entry->src_caller_lr = 0;
                    entry->src_frame_seq = 0;
                }
                const char* tag = classify_address_unlocked(word_ea, NULL);
                copy_text(entry->alloc_tag, sizeof(entry->alloc_tag), tag);
            }

            if (processed_words == 0) {
                first_value = value;
                first_src_ea = source_ea;
            }
            processed_words++;
        }
    }

    if (processed_words > 0) {
        uint8_t history_width = size > 255u ? 255u : (uint8_t)size;
        append_history_event(dst_ea, first_value, history_width,
                             is_copy ? 1u : 0u, pc, lr, frame_seq,
                             first_src_ea);
    }
    return processed_words;
}

void m2_provenance_record_write(uint32_t ea, uint32_t value, uint8_t width,
                                uint32_t pc, uint32_t lr, uint32_t frame_seq) {
    if (!m2_provenance_active || !s_prov_state.log_writes || width == 0) return;
    M2GuestEvent event;
    const char* event_tag = NULL;
    bool emit = false;
    pthread_mutex_lock(&s_prov_lock);
    if (is_watched_unlocked(ea, width)) {
        M2ProvEntry* e = get_or_create_entry_unlocked(ea);
        if (e) {
            e->latest_value = value;
            e->writer_pc = pc;
            e->caller_lr = lr;
            e->frame_seq = frame_seq;
            e->write_count++;
            e->last_width = width;
            e->last_size = width;
            e->src_ea = 0;
            e->copy_pc = 0;
            e->src_writer_pc = 0;
            e->src_caller_lr = 0;
            e->src_frame_seq = 0;
            const char* tag = classify_address_unlocked(ea, NULL);
            copy_text(e->alloc_tag, sizeof(e->alloc_tag), tag);
            event_tag = e->alloc_tag;
        }
        append_history_event(ea, value, width, 0u, pc, lr, frame_seq, 0u);
        init_provenance_event(&event, M2_GUEST_EVENT_WRITE, ea, 0u, width,
                              value, width, pc, lr, frame_seq, event_tag,
                              classify_address_unlocked(ea, NULL));
        emit = true;
    }
    pthread_mutex_unlock(&s_prov_lock);
    if (emit) emit_guest_event(&event);
}

void m2_provenance_record_read(uint32_t ea, uint32_t value, uint8_t width,
                               uint32_t pc, uint32_t lr, uint32_t frame_seq) {
    if (!m2_provenance_active || !m2_provenance_reads_active ||
        width == 0) return;
    M2GuestEvent event;
    bool emit = false;
    pthread_mutex_lock(&s_prov_lock);
    if (is_watched_unlocked(ea, width)) {
        append_history_event(ea, value, width, 4u, pc, lr, frame_seq, 0u);
        const M2ProvEntry* entry = find_entry_unlocked(ea);
        init_provenance_event(&event, M2_GUEST_EVENT_READ, ea, 0u, width,
                              value, width, pc, lr, frame_seq, NULL,
                              classify_address_unlocked(ea, NULL));
        if (entry) copy_text(event.tag, sizeof(event.tag), entry->alloc_tag);
        emit = true;
    }
    pthread_mutex_unlock(&s_prov_lock);
    if (emit) emit_guest_event(&event);
}

void m2_provenance_record_host_write(const uint8_t* guest_mem, uint32_t ea,
                                     uint32_t size, uint32_t pc, uint32_t lr,
                                     uint32_t frame_seq) {
    if (!m2_provenance_active || !s_prov_state.log_writes || size == 0) return;
    M2GuestEvent event;
    bool emit = false;
    pthread_mutex_lock(&s_prov_lock);
    uint32_t processed = record_memory_operation_unlocked(
        guest_mem, false, ea, 0u, size, pc, lr, frame_seq);
    if (processed > 0) {
        init_provenance_event(&event, M2_GUEST_EVENT_WRITE, ea, 0u, size,
                              guest_mem ? read_guest_word(guest_mem, ea) : 0u,
                              size > 255u ? 255u : size, pc, lr, frame_seq,
                              NULL, classify_address_unlocked(ea, NULL));
        emit = true;
    }
    pthread_mutex_unlock(&s_prov_lock);
    if (emit) emit_guest_event(&event);
}

void m2_provenance_record_copy(uint32_t dst_ea, uint32_t src_ea,
                               uint32_t size, uint32_t pc, uint32_t lr,
                               uint32_t frame_seq) {
    if (!m2_provenance_active || !s_prov_state.log_copies || size == 0) return;
    M2GuestEvent event;
    bool emit = false;
    pthread_mutex_lock(&s_prov_lock);
    uint32_t processed = record_memory_operation_unlocked(
        NULL, true, dst_ea, src_ea, size, pc, lr, frame_seq);
    if (processed > 0) {
        init_provenance_event(&event, M2_GUEST_EVENT_COPY, dst_ea, src_ea,
                              size, 0u, size > 255u ? 255u : size, pc, lr,
                              frame_seq, NULL,
                              classify_address_unlocked(dst_ea, NULL));
        emit = true;
    }
    pthread_mutex_unlock(&s_prov_lock);
    if (emit) emit_guest_event(&event);
}

void m2_provenance_record_copy_from_guest(const uint8_t* guest_mem,
                                          uint32_t dst_ea, uint32_t src_ea,
                                          uint32_t size, uint32_t pc,
                                          uint32_t lr, uint32_t frame_seq) {
    if (!m2_provenance_active || !s_prov_state.log_copies || size == 0) return;
    M2GuestEvent event;
    bool emit = false;
    pthread_mutex_lock(&s_prov_lock);
    uint32_t processed = record_memory_operation_unlocked(
        guest_mem, true, dst_ea, src_ea, size, pc, lr, frame_seq);
    if (processed > 0) {
        init_provenance_event(
            &event, M2_GUEST_EVENT_COPY, dst_ea, src_ea, size,
            guest_mem ? read_guest_word(guest_mem, dst_ea) : 0u,
            size > 255u ? 255u : size, pc, lr, frame_seq, NULL,
            classify_address_unlocked(dst_ea, NULL));
        emit = true;
    }
    pthread_mutex_unlock(&s_prov_lock);
    if (emit) emit_guest_event(&event);
}

void m2_provenance_record_alloc(uint32_t start_ea, uint32_t size, uint32_t pc, const char* tag, uint32_t frame_seq) {
    if (!m2_provenance_active || !s_prov_state.enabled || !s_prov_state.log_allocs) return;
    if (size == 0) size = 1;

    M2GuestEvent event;
    char event_tag[M2_GUEST_EVENT_TEXT_MAX];
    event_tag[0] = '\0';
    pthread_mutex_lock(&s_prov_lock);
    if (s_prov_state.num_allocs < M2_PROV_MAX_ALLOC_RECORDS) {
        M2ProvAllocRecord* a = &s_prov_state.allocs[s_prov_state.num_allocs++];
        memset(a, 0, sizeof(*a));
        a->start_ea = start_ea;
        a->size = size;
        a->end_ea = (uint32_t)inclusive_end(start_ea, size);
        a->alloc_pc = pc;
        a->frame_seq = frame_seq;
        a->alloc_id = (uint32_t)s_prov_state.num_allocs;
        a->active = true;
        if (tag) {
            copy_text(a->tag, sizeof(a->tag), tag);
        } else {
            snprintf(a->tag, sizeof(a->tag), "Alloc#%u", a->alloc_id);
        }
        copy_text(event_tag, sizeof(event_tag), a->tag);
    }
    append_history_event(start_ea, size, 4, 2 /* alloc */, pc, 0, frame_seq, 0);
    init_provenance_event(&event, M2_GUEST_EVENT_ALLOC, start_ea, 0u, size,
                          size, 4u, pc, 0u, frame_seq, event_tag,
                          classify_address_unlocked(start_ea, NULL));
    pthread_mutex_unlock(&s_prov_lock);
    emit_guest_event(&event);
}

void m2_provenance_record_free(uint32_t start_ea, uint32_t size, uint32_t pc, uint32_t frame_seq) {
    if (!m2_provenance_active || !s_prov_state.enabled || !s_prov_state.log_allocs) return;
    M2GuestEvent event;
    pthread_mutex_lock(&s_prov_lock);
    for (size_t i = 0; i < s_prov_state.num_allocs; i++) {
        if (s_prov_state.allocs[i].start_ea == start_ea && s_prov_state.allocs[i].active) {
            s_prov_state.allocs[i].active = false;
            break;
        }
    }
    append_history_event(start_ea, 0, 4, 3 /* free */, pc, 0, frame_seq, 0);
    init_provenance_event(&event, M2_GUEST_EVENT_FREE, start_ea, 0u, size,
                          0u, 4u, pc, 0u, frame_seq, NULL,
                          classify_address_unlocked(start_ea, NULL));
    pthread_mutex_unlock(&s_prov_lock);
    emit_guest_event(&event);
}

void m2_provenance_report_oob(uint32_t oob_ea, uint8_t width, uint32_t pc, uint32_t lr) {
    if (!m2_provenance_active) return;
    M2ProvAllocRecord alloc_info;
    M2ProvEntry entry_copy;
    char classification[64];
    bool have_entry = false;

    pthread_mutex_lock(&s_prov_lock);
    if (!s_prov_state.auto_oob_dump || s_prov_oob_reports >= 64u) {
        pthread_mutex_unlock(&s_prov_lock);
        return;
    }
    s_prov_oob_reports++;
    const char* classification_text =
        classify_address_unlocked(oob_ea, &alloc_info);
    copy_text(classification, sizeof(classification), classification_text);
    const M2ProvEntry* entry = find_entry_unlocked(oob_ea);
    if (entry) {
        entry_copy = *entry;
        have_entry = true;
    }
    pthread_mutex_unlock(&s_prov_lock);

    M2GuestEvent event;
    init_provenance_event(&event, M2_GUEST_EVENT_OOB, oob_ea, 0u, width,
                          have_entry ? entry_copy.latest_value : 0u, width,
                          pc, lr, have_entry ? entry_copy.frame_seq : 0u,
                          NULL, classification);
    emit_guest_event(&event);

    fprintf(stderr, "[prov-oob] FAULT pc=%#x lr=%#x ea=%#x (width=%u) region='%s'\n",
            pc, lr, oob_ea, width, classification);
    if (have_entry) {
        float fval = 0.0f;
        memcpy(&fval, &entry_copy.latest_value, sizeof(float));
        fprintf(stderr, "[prov-oob]   entry ea=%#x latest_val=%#010x (f32=%f) writer_pc=%#x lr=%#x writes=%u\n",
                entry_copy.ea, entry_copy.latest_value, fval,
                entry_copy.writer_pc, entry_copy.caller_lr,
                entry_copy.write_count);
        if (entry_copy.src_ea) {
            fprintf(stderr,
                    "[prov-oob]   copied from src_ea=%#x copy_pc=%#x src_writer_pc=%#x\n",
                    entry_copy.src_ea, entry_copy.copy_pc,
                    entry_copy.src_writer_pc);
        }
    }
}

static void json_write_string(FILE* file, const char* value) {
    fputc('"', file);
    if (value) {
        for (const unsigned char* cursor = (const unsigned char*)value;
             *cursor; cursor++) {
            switch (*cursor) {
                case '"': fputs("\\\"", file); break;
                case '\\': fputs("\\\\", file); break;
                case '\b': fputs("\\b", file); break;
                case '\f': fputs("\\f", file); break;
                case '\n': fputs("\\n", file); break;
                case '\r': fputs("\\r", file); break;
                case '\t': fputs("\\t", file); break;
                default:
                    if (*cursor < 0x20) {
                        fprintf(file, "\\u%04x", *cursor);
                    } else {
                        fputc(*cursor, file);
                    }
                    break;
            }
        }
    }
    fputc('"', file);
}

bool m2_provenance_dump_json(const char* filepath) {
    if (!filepath || !*filepath) return false;
    ensure_parent_dir(filepath);
    FILE* f = fopen(filepath, "w");
    if (!f) {
        fprintf(stderr, "[prov] Failed to open %s for JSON export\n", filepath);
        return false;
    }

    pthread_mutex_lock(&s_prov_lock);
    fprintf(f, "{\n");
    fprintf(f, "  \"schema_version\": %u,\n", M2_GUEST_EVENT_SCHEMA_VERSION);
    fprintf(f, "  \"event_format\": \"m2-guest-events-v1\",\n");
    fprintf(f, "  \"config\": {\n");
    fprintf(f, "    \"enabled\": %s,\n", s_prov_state.enabled ? "true" : "false");
    fprintf(f, "    \"log_writes\": %s,\n", s_prov_state.log_writes ? "true" : "false");
    fprintf(f, "    \"log_reads\": %s,\n", s_prov_state.log_reads ? "true" : "false");
    fprintf(f, "    \"log_copies\": %s,\n", s_prov_state.log_copies ? "true" : "false");
    fprintf(f, "    \"log_allocs\": %s,\n", s_prov_state.log_allocs ? "true" : "false");
    fprintf(f, "    \"watched_ranges\": [\n");
    for (size_t i = 0; i < s_prov_state.num_watched_ranges; i++) {
        const M2ProvWatchedRange* r = &s_prov_state.watched_ranges[i];
        fprintf(f, "      {\"start\": \"0x%08x\", \"end\": \"0x%08x\", \"label\": ",
                r->start_ea, r->end_ea);
        json_write_string(f, r->label);
        fprintf(f, "}%s\n",
                (i + 1 < s_prov_state.num_watched_ranges) ? "," : "");
    }
    fprintf(f, "    ]\n");
    fprintf(f, "  },\n");

    // Entries
    fprintf(f, "  \"entries\": [\n");
    for (size_t i = 0; i < s_prov_state.num_entries; i++) {
        const M2ProvEntry* e = &s_prov_state.entries[i];
        float fval = 0.0f;
        memcpy(&fval, &e->latest_value, sizeof(float));
        int32_t ival = (int32_t)e->latest_value;
        fprintf(f, "    {\n");
        fprintf(f, "      \"ea\": \"0x%08x\",\n", e->ea);
        fprintf(f, "      \"value_hex\": \"0x%08x\",\n", e->latest_value);
        fprintf(f, "      \"value_u32\": %u,\n", e->latest_value);
        fprintf(f, "      \"value_i32\": %d,\n", ival);
        if (isfinite(fval)) {
            fprintf(f, "      \"value_f32\": %f,\n", fval);
        } else {
            fprintf(f, "      \"value_f32\": null,\n");
        }
        fprintf(f, "      \"writer_pc\": \"0x%08x\",\n", e->writer_pc);
        fprintf(f, "      \"caller_lr\": \"0x%08x\",\n", e->caller_lr);
        fprintf(f, "      \"frame_seq\": %u,\n", e->frame_seq);
        fprintf(f, "      \"write_count\": %u,\n", e->write_count);
        fprintf(f, "      \"width\": %u,\n", e->last_width);
        fprintf(f, "      \"size\": %u,\n", e->last_size);
        fprintf(f, "      \"src_ea\": \"0x%08x\",\n", e->src_ea);
        fprintf(f, "      \"copy_pc\": \"0x%08x\",\n", e->copy_pc);
        fprintf(f, "      \"src_writer_pc\": \"0x%08x\",\n", e->src_writer_pc);
        fprintf(f, "      \"src_caller_lr\": \"0x%08x\",\n", e->src_caller_lr);
        fprintf(f, "      \"src_frame_seq\": %u,\n", e->src_frame_seq);
        fprintf(f, "      \"alloc_tag\": ");
        json_write_string(f, e->alloc_tag);
        fprintf(f, "\n");
        fprintf(f, "    }%s\n", (i + 1 < s_prov_state.num_entries) ? "," : "");
    }
    fprintf(f, "  ],\n");

    // Allocations
    fprintf(f, "  \"allocations\": [\n");
    for (size_t i = 0; i < s_prov_state.num_allocs; i++) {
        const M2ProvAllocRecord* a = &s_prov_state.allocs[i];
        fprintf(f, "    {\n");
        fprintf(f, "      \"id\": %u,\n", a->alloc_id);
        fprintf(f, "      \"start_ea\": \"0x%08x\",\n", a->start_ea);
        fprintf(f, "      \"end_ea\": \"0x%08x\",\n", a->end_ea);
        fprintf(f, "      \"size\": %u,\n", a->size);
        fprintf(f, "      \"alloc_pc\": \"0x%08x\",\n", a->alloc_pc);
        fprintf(f, "      \"frame_seq\": %u,\n", a->frame_seq);
        fprintf(f, "      \"tag\": ");
        json_write_string(f, a->tag);
        fprintf(f, ",\n");
        fprintf(f, "      \"active\": %s\n", a->active ? "true" : "false");
        fprintf(f, "    }%s\n", (i + 1 < s_prov_state.num_allocs) ? "," : "");
    }
    fprintf(f, "  ],\n");

    // History ring
    fprintf(f, "  \"history\": [\n");
    size_t count = s_prov_state.history_count;
    size_t start_idx = (s_prov_state.history_count < M2_PROV_MAX_HISTORY) ? 0 : s_prov_state.history_head;
    for (size_t i = 0; i < count; i++) {
        size_t idx = (start_idx + i) % M2_PROV_MAX_HISTORY;
        const M2ProvEvent* ev = &s_prov_state.history[idx];
        const char* op_str = (ev->op_type == 0) ? "write" :
                             (ev->op_type == 1) ? "copy" :
                             (ev->op_type == 2) ? "alloc" :
                             (ev->op_type == 3) ? "free" : "read";
        fprintf(f, "    {\"ea\": \"0x%08x\", \"op\": \"%s\", \"value\": \"0x%08x\", \"pc\": \"0x%08x\", \"lr\": \"0x%08x\", \"frame\": %u, \"width\": %u, \"src_ea\": \"0x%08x\"}%s\n",
                ev->ea, op_str, ev->value, ev->pc, ev->lr, ev->frame_seq, ev->width, ev->src_ea,
                (i + 1 < count) ? "," : "");
    }
    fprintf(f, "  ]\n");
    fprintf(f, "}\n");

    pthread_mutex_unlock(&s_prov_lock);
    fclose(f);
    return true;
}
