// shim_stub.c — minimal m2_selector_lookup for standalone recomp builds.
// The weak definition here satisfies generated bl sites without pulling in the
// host folio_loader (which needs SDL). When the host links the runtime, the
// strong definition in host/core/folio_loader.cpp wins.

#include "shim_abi.h"
#include <stdio.h>
#include <stdint.h>

static void stub_noop(void) {
    // Intentionally silent; coverage comes from recomp_report / host --headless report.
}

__attribute__((weak)) void* m2_selector_lookup(uint32_t folio, uint32_t ordinal) {
    (void)folio; (void)ordinal;
    return (void*)stub_noop;
}

// Standalone fallback for the optional guest CLT submission bridge. The
// production host supplies the strong renderer callback; the guest kernel IO
// semantics remain unchanged when no renderer is linked.
__attribute__((weak)) void m2_guest_graphics_submit_c(
    uint32_t io_item, uint32_t command, uint32_t command_options,
    uint32_t guest_buffer, uint32_t byte_count,
    const uint8_t* guest_bytes,
    const uint8_t* guest_memory, uint32_t guest_memory_size) {
    (void)io_item;
    (void)command;
    (void)command_options;
    (void)guest_buffer;
    (void)byte_count;
    (void)guest_bytes;
    (void)guest_memory;
    (void)guest_memory_size;
}

// Standalone fallback for the optional filesystem directory bridge. The
// production host supplies strong definitions from filesystem_shim.cpp, and
// focused filesystem tests provide their own overrides. Returning the same
// fail-closed values as an absent bridge keeps standalone links portable on
// Mach-O, where an undefined weak reference is still a link error.
__attribute__((weak)) uint32_t m2_host_dir_open(const char* path) {
    (void)path;
    return 0u;
}

__attribute__((weak)) int m2_host_dir_next(uint32_t token, char* out,
                                           uint32_t cap) {
    (void)token; (void)out; (void)cap;
    return -1;
}

__attribute__((weak)) void m2_host_dir_close(uint32_t token) {
    (void)token;
}

// Standalone fallback for the folio 4:7 ChangeDirectory bridge. Same rule as
// the directory/raw-file bridges above: the production host supplies a strong
// definition from filesystem_shim.cpp, and a standalone link keeps
// ChangeDirectory failing closed ("nothing matched") rather than claiming a
// directory that was never checked.
__attribute__((weak)) int m2_host_path_kind(const char* path) {
    (void)path;
    return 0;
}

// Standalone fallback for the filesystem raw-file bridge (folio 4: 35/36/37/40).
// The production host supplies strong definitions from filesystem_shim.cpp, and
// the focused m2_filesystem_raw_test provides its own overrides. Returning
// fail-closed values keeps the standalone link portable on Mach-O, where an
// undefined weak reference is still a link error.
__attribute__((weak)) uint32_t m2_host_raw_open(const char* path) {
    (void)path;
    return 0u;
}
__attribute__((weak)) uint32_t m2_host_raw_size(uint32_t token) {
    (void)token;
    return 0u;
}
__attribute__((weak)) int32_t m2_host_raw_read(uint32_t token, void* dst,
                                              uint32_t n) {
    (void)token; (void)dst; (void)n;
    return -1;
}
__attribute__((weak)) int32_t m2_host_raw_read_at(uint32_t token,
                                                  uint64_t offset,
                                                  void* dst, uint32_t n) {
    (void)token; (void)offset; (void)dst; (void)n;
    return -1;
}
__attribute__((weak)) int32_t m2_host_raw_write(uint32_t token, const void* src,
                                               uint32_t n) {
    (void)token; (void)src; (void)n;
    return -1;
}
__attribute__((weak)) int32_t m2_host_raw_seek(uint32_t token, int32_t offset,
                                              int32_t whence) {
    (void)token; (void)offset; (void)whence;
    return -1;
}
__attribute__((weak)) void m2_host_raw_close(uint32_t token) {
    (void)token;
}

__attribute__((weak)) int m2_decode_mpeg_still(const uint8_t* bitstream,
                                               size_t bitstream_size,
                                               uint8_t* dst_buffer,
                                               size_t dst_buffer_size,
                                               int width, int height) {
    (void)bitstream; (void)bitstream_size; (void)dst_buffer; (void)dst_buffer_size;
    (void)width; (void)height;
    return -1;
}

__attribute__((weak)) int m2_mpeg_video_feed(const uint8_t* data, size_t len,
                                             uint32_t flags, uint32_t pts,
                                             uint32_t user_data) {
    (void)data; (void)len; (void)flags; (void)pts; (void)user_data;
    return -1;
}

__attribute__((weak)) int m2_mpeg_video_configure_buffers(
    uint32_t ref_min_size, uint32_t strip_min_size,
    uint32_t ref_client_control, uint32_t strip_client_control) {
    (void)ref_min_size;
    (void)strip_min_size;
    (void)ref_client_control;
    (void)strip_client_control;
    return 0;
}

__attribute__((weak)) int m2_mpeg_video_has_frame(void) {
    return 0;
}

__attribute__((weak)) int m2_mpeg_video_pop_frame(uint8_t* dst_buf, size_t dst_len,
                                                  uint32_t* pts_out, uint32_t* user_data_out) {
    (void)dst_buf; (void)dst_len; (void)pts_out; (void)user_data_out;
    return 0;
}

__attribute__((weak)) void m2_mpeg_video_flush(void) {}

__attribute__((weak)) void m2_mpeg_video_reset(void) {}

__attribute__((weak)) unsigned g_recomp_text_base = 0;
__attribute__((weak)) unsigned g_recomp_text_size = 0;
__attribute__((weak)) unsigned g_recomp_data_base = 0;
__attribute__((weak)) unsigned g_recomp_data_size = 0;
__attribute__((weak)) unsigned g_recomp_bss_base = 0;
__attribute__((weak)) unsigned g_recomp_bss_size = 0;
__attribute__((weak)) uint64_t _m2_oob_total = 0;
