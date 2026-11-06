// Device-command accepted set — see include/device_command_contract.h for
// the invariant, the vocabulary and the refusal identities.
//
// Each row follows M2 SDK src/includes/kernel/devicecmd.h; documentation
// provenance for each class is that command table itself.

#include "device_command_contract.h"

#include <stddef.h>

// The argument-form classifier answers with the host kernel's own status
// convention (M2K_OK / M2K_ERR_*), the same one the shared send/do paths return.
#include "guest_kernel.h"

static const m2_device_command_entry g_m2_device_commands[] = {
    {0x00000000u, "CMD_WRITE", M2_DEVCMD_UNMODELLED},
    {0x00000001u, "CMD_READ", M2_DEVCMD_HANDLED},
    {0x00000002u, "CMD_STATUS", M2_DEVCMD_HANDLED},
    {0x000000c0u, "CMD_GETMAPINFO", M2_DEVCMD_UNMODELLED},
    {0x000000c1u, "CMD_MAPRANGE", M2_DEVCMD_UNMODELLED},
    {0x000000c2u, "CMD_UNMAPRANGE", M2_DEVCMD_UNMODELLED},
    {0x000000c3u, "CMD_BLOCKREAD", M2_DEVCMD_HANDLED},
    {0x000000c4u, "CMD_BLOCKWRITE", M2_DEVCMD_UNMODELLED},
    {0x000000c5u, "CMD_STREAMREAD", M2_DEVCMD_UNMODELLED},
    {0x000000c6u, "CMD_STREAMWRITE", M2_DEVCMD_UNMODELLED},
    {0x000000c7u, "CMD_GETICON", M2_DEVCMD_UNMODELLED},
    {0x000000c8u, "CMD_PREFER_FSTYPE", M2_DEVCMD_UNMODELLED},
    {0x000000c9u, "CMD_STREAMFLUSH", M2_DEVCMD_UNMODELLED},
    {0x00024000u, "GFXCMD_PROJECTORMODULE", M2_DEVCMD_UNMODELLED},
    {0x00030003u, "FILECMD_READDIR", M2_DEVCMD_UNMODELLED},
    {0x00030004u, "FILECMD_GETPATH", M2_DEVCMD_UNMODELLED},
    {0x00030005u, "FILECMD_READENTRY", M2_DEVCMD_UNMODELLED},
    {0x00030006u, "FILECMD_ALLOCBLOCKS", M2_DEVCMD_UNMODELLED},
    {0x00030007u, "FILECMD_SETEOF", M2_DEVCMD_UNMODELLED},
    {0x00030008u, "FILECMD_ADDENTRY", M2_DEVCMD_UNMODELLED},
    {0x00030009u, "FILECMD_DELETEENTRY", M2_DEVCMD_UNMODELLED},
    {0x0003000au, "FILECMD_SETTYPE", M2_DEVCMD_UNMODELLED},
    {0x0003000bu, "FILECMD_OPENENTRY", M2_DEVCMD_UNMODELLED},
    {0x0003000cu, "FILECMD_FSSTAT", M2_DEVCMD_UNMODELLED},
    {0x0003000du, "FILECMD_ADDDIR", M2_DEVCMD_UNMODELLED},
    {0x0003000eu, "FILECMD_DELETEDIR", M2_DEVCMD_UNMODELLED},
    {0x0003000fu, "FILECMD_SETVERSION", M2_DEVCMD_UNMODELLED},
    {0x00030010u, "FILECMD_SETBLOCKSIZE", M2_DEVCMD_UNMODELLED},
    {0x00030011u, "FILECMD_SETFLAGS", M2_DEVCMD_UNMODELLED},
    {0x00030012u, "FILECMD_RENAME", M2_DEVCMD_UNMODELLED},
    {0x00030013u, "FILECMD_SETDATE", M2_DEVCMD_UNMODELLED},
    {0x00040003u, "TIMERCMD_GETTIME_VBL", M2_DEVCMD_HANDLED},
    {0x00040004u, "TIMERCMD_SETTIME_VBL", M2_DEVCMD_UNMODELLED},
    {0x00040005u, "TIMERCMD_DELAY_VBL", M2_DEVCMD_HANDLED},
    {0x00040006u, "TIMERCMD_DELAYUNTIL_VBL", M2_DEVCMD_HANDLED},
    {0x00040007u, "TIMERCMD_METRONOME_VBL", M2_DEVCMD_HANDLED},
    {0x00040008u, "TIMERCMD_GETTIME_USEC", M2_DEVCMD_HANDLED},
    {0x00040009u, "TIMERCMD_SETTIME_USEC", M2_DEVCMD_HANDLED},
    {0x0004000au, "TIMERCMD_DELAY_USEC", M2_DEVCMD_HANDLED},
    {0x0004000bu, "TIMERCMD_DELAYUNTIL_USEC", M2_DEVCMD_HANDLED},
    {0x0004000cu, "TIMERCMD_METRONOME_USEC", M2_DEVCMD_HANDLED},
    {0x00050003u, "CPORT_CMD_CONFIGURE", M2_DEVCMD_UNMODELLED},
    {0x00050004u, "CPORT_CMD_READEVENT", M2_DEVCMD_UNMODELLED},
    {0x00050005u, "CPORT_CMD_WRITEEVENT", M2_DEVCMD_UNMODELLED},
    {0x00050006u, "CPORT_CMD_WRITE", M2_DEVCMD_UNMODELLED},
    {0x00050007u, "CPORT_CMD_READ", M2_DEVCMD_UNMODELLED},
    {0x00060003u, "CDROMCMD_READ", M2_DEVCMD_UNMODELLED},
    {0x00060004u, "CDROMCMD_SCAN_READ", M2_DEVCMD_UNMODELLED},
    {0x00060005u, "CDROMCMD_SETDEFAULTS", M2_DEVCMD_UNMODELLED},
    {0x00060006u, "CDROMCMD_DISCDATA", M2_DEVCMD_UNMODELLED},
    {0x00060007u, "CDROMCMD_READ_SUBQ", M2_DEVCMD_UNMODELLED},
    {0x00060008u, "CDROMCMD_OPEN_DRAWER", M2_DEVCMD_UNMODELLED},
    {0x00060009u, "CDROMCMD_CLOSE_DRAWER", M2_DEVCMD_UNMODELLED},
    {0x0006000au, "CDROMCMD_WOBBLE_INFO", M2_DEVCMD_UNMODELLED},
    {0x0006000bu, "CDROMCMD_RESIZE_BUFFERS", M2_DEVCMD_UNMODELLED},
    {0x0006000cu, "CDROMCMD_DIAG_INFO", M2_DEVCMD_UNMODELLED},
    {0x0006000du, "CDROMCMD_MONITOR", M2_DEVCMD_UNMODELLED},
    {0x0006000eu, "CDROMCMD_PASSTHRU", M2_DEVCMD_UNMODELLED},
    {0x00070003u, "MPEGVIDEOCMD_CONTROL", M2_DEVCMD_HANDLED},
    {0x00070004u, "MPEGVIDEOCMD_WRITE", M2_DEVCMD_HANDLED},
    {0x00070005u, "MPEGVIDEOCMD_READ", M2_DEVCMD_HANDLED},
    {0x00070006u, "MPEGVIDEOCMD_GETBUFFERINFO", M2_DEVCMD_HANDLED},
    {0x00070007u, "MPEGVIDEOCMD_SETSTRIPBUFFER", M2_DEVCMD_HANDLED},
    {0x00070008u, "MPEGVIDEOCMD_SETREFERENCEBUFFER", M2_DEVCMD_HANDLED},
    {0x00070009u, "MPEGVIDEOCMD_ALLOCBUFFERS", M2_DEVCMD_HANDLED},
    {0x00090003u, "PROXY_CMD_CREATE_DEVICE", M2_DEVCMD_UNMODELLED},
    {0x00090004u, "PROXY_CMD_DWIM", M2_DEVCMD_UNMODELLED},
    {0x00090005u, "PROXY_CMD_CREATE_SOFTFIRQ", M2_DEVCMD_UNMODELLED},
    {0x000a0003u, "USLOTCMD_RESET", M2_DEVCMD_UNMODELLED},
    {0x000a0004u, "USLOTCMD_SETCLOCK", M2_DEVCMD_UNMODELLED},
    {0x000a0008u, "USLOTCMD_SEQ", M2_DEVCMD_UNMODELLED},
    {0x000b0005u, "SLOTCMD_SETTIMING", M2_DEVCMD_UNMODELLED},
    {0x000c0003u, "SER_CMD_STATUS", M2_DEVCMD_UNMODELLED},
    {0x000c0004u, "SER_CMD_SETCONFIG", M2_DEVCMD_UNMODELLED},
    {0x000c0005u, "SER_CMD_GETCONFIG", M2_DEVCMD_UNMODELLED},
    {0x000c0006u, "SER_CMD_WAITEVENT", M2_DEVCMD_UNMODELLED},
    {0x000c0007u, "SER_CMD_SETRTS", M2_DEVCMD_UNMODELLED},
    {0x000c0008u, "SER_CMD_SETDTR", M2_DEVCMD_UNMODELLED},
    {0x000c0009u, "SER_CMD_SETLOOPBACK", M2_DEVCMD_UNMODELLED},
    {0x000c000au, "SER_CMD_BREAK", M2_DEVCMD_UNMODELLED},
    {0x000d0003u, "TE_CMD_EXECUTELIST", M2_DEVCMD_HANDLED},
    {0x000d0004u, "TE_CMD_SETVIEW", M2_DEVCMD_HANDLED},
    {0x000d0005u, "TE_CMD_SETZBUFFER", M2_DEVCMD_HANDLED},
    {0x000d0006u, "TE_CMD_SETFRAMEBUFFER", M2_DEVCMD_HANDLED},
    {0x000d0007u, "TE_CMD_DISPLAYFRAMEBUFFER", M2_DEVCMD_HANDLED},
    {0x000d0008u, "TE_CMD_STEP", M2_DEVCMD_HANDLED},
    {0x000d0009u, "TE_CMD_SPEEDCONTROL", M2_DEVCMD_HANDLED},
    {0x000d000au, "TE_CMD_SETVBLABORTCOUNT", M2_DEVCMD_HANDLED},
    {0x000d0100u, "TE_CMD_GETIDLETIME", M2_DEVCMD_HANDLED},
    {0x01030003u, "HOST_CMD_SEND", M2_DEVCMD_UNMODELLED},
    {0x01030004u, "HOST_CMD_RECV", M2_DEVCMD_UNMODELLED},
    {0x01040003u, "HOSTFS_CMD_MOUNTFS", M2_DEVCMD_UNMODELLED},
    {0x01040004u, "HOSTFS_CMD_OPENENTRY", M2_DEVCMD_UNMODELLED},
    {0x01040005u, "HOSTFS_CMD_CLOSEENTRY", M2_DEVCMD_UNMODELLED},
    {0x01040006u, "HOSTFS_CMD_CREATEFILE", M2_DEVCMD_UNMODELLED},
    {0x01040007u, "HOSTFS_CMD_CREATEDIR", M2_DEVCMD_UNMODELLED},
    {0x01040008u, "HOSTFS_CMD_DELETEENTRY", M2_DEVCMD_UNMODELLED},
    {0x01040009u, "HOSTFS_CMD_READENTRY", M2_DEVCMD_UNMODELLED},
    {0x0104000au, "HOSTFS_CMD_READDIR", M2_DEVCMD_UNMODELLED},
    {0x0104000bu, "HOSTFS_CMD_ALLOCBLOCKS", M2_DEVCMD_UNMODELLED},
    {0x0104000cu, "HOSTFS_CMD_BLOCKREAD", M2_DEVCMD_UNMODELLED},
    {0x0104000du, "HOSTFS_CMD_STATUS", M2_DEVCMD_UNMODELLED},
    {0x0104000eu, "HOSTFS_CMD_FSSTAT", M2_DEVCMD_UNMODELLED},
    {0x0104000fu, "HOSTFS_CMD_BLOCKWRITE", M2_DEVCMD_UNMODELLED},
    {0x01040010u, "HOSTFS_CMD_SETEOF", M2_DEVCMD_UNMODELLED},
    {0x01040011u, "HOSTFS_CMD_SETTYPE", M2_DEVCMD_UNMODELLED},
    {0x01040012u, "HOSTFS_CMD_SETVERSION", M2_DEVCMD_UNMODELLED},
    {0x01040013u, "HOSTFS_CMD_DISMOUNTFS", M2_DEVCMD_UNMODELLED},
    {0x01040014u, "HOSTFS_CMD_SETBLOCKSIZE", M2_DEVCMD_UNMODELLED},
    {0x01040015u, "HOSTFS_CMD_RENAMEENTRY", M2_DEVCMD_UNMODELLED},
    {0x01040016u, "HOSTFS_CMD_SETDATE", M2_DEVCMD_UNMODELLED},
    {0x01050003u, "HOSTCONSOLE_CMD_GETCMDLINE", M2_DEVCMD_UNMODELLED},
    {0x01060003u, "MP_CMD_DISPATCH", M2_DEVCMD_HANDLED},
};

static const uint32_t g_m2_device_command_count =
    (uint32_t)(sizeof(g_m2_device_commands) /
               sizeof(g_m2_device_commands[0]));

static const m2_device_command_entry* find_entry(uint32_t command) {
    // The table is sorted by command word; a bounded linear scan keeps the
    // lookup allocation-free and is short enough to stay in cache.
    for (uint32_t i = 0; i < g_m2_device_command_count; ++i) {
        if (g_m2_device_commands[i].command == command)
            return &g_m2_device_commands[i];
    }
    return NULL;
}

m2_device_command_disposition m2_device_command_lookup(uint32_t command) {
    const m2_device_command_entry* entry = find_entry(command);
    if (!entry) return M2_DEVCMD_UNDOCUMENTED;
    return entry->disposition;
}

const char* m2_device_command_name(uint32_t command) {
    const m2_device_command_entry* entry = find_entry(command);
    return entry ? entry->name : "";
}

const m2_device_command_entry* m2_device_command_table(uint32_t* count_out) {
    if (count_out) *count_out = g_m2_device_command_count;
    return g_m2_device_commands;
}

// ---- documented argument forms -------------------------------------------
//
// One row per documented argument form of an accepted command, with the host's
// disposition for it. The vocabulary rows the same forms; keep the two sets
// equal so a form is never added on one side only. The TE commands keep their own
// form-level denominator (the TE command vocabulary,
// runtime/include/te_command_contract.h), which the vocabulary names.
static const m2_device_form_entry g_m2_device_forms[] = {
    // CMD_READ (generic, obsolete: "use CMD_xxxREAD below"). The file pool
    // serves its own read command; the FMV subscriber read is the shape the
    // host models.
    {0x00000001u, "read_file_device", M2_DEVFORM_REFUSED},
    {0x00000001u, "read_fmv_still_frame", M2_DEVFORM_MODELED},
    {0x00000001u, "read_other_device", M2_DEVFORM_REFUSED},
    // FileDriverStatus copies min(requested, sizeof(FileStatus)), including a
    // short prefix. Other devices retain their own minimum record contract.
    {0x00000002u, "status_file_device_truncated_record", M2_DEVFORM_MODELED},
    {0x00000002u, "status_file_device_record", M2_DEVFORM_MODELED},
    {0x00000002u, "status_nonfile_device_record", M2_DEVFORM_MODELED},
    {0x00000002u, "status_nonfile_device_out_of_range", M2_DEVFORM_REFUSED},
    {0x00000002u, "status_nonfile_device_short_record", M2_DEVFORM_REFUSED},
    // CMD_BLOCKREAD: the documentation allows a length that is not a multiple
    // of the device block size ("less data may be returned"), which the host
    // does not model; it fails closed instead.
    {0x000000c3u, "blockread_file_block_multiple", M2_DEVFORM_MODELED},
    {0x000000c3u, "blockread_file_partial_block_length", M2_DEVFORM_REFUSED},
    {0x000000c3u, "blockread_nonfile_device", M2_DEVFORM_REFUSED},
    // TIMERCMD_METRONOME_USEC: TimerDriver validates the signal bits and the
    // eight-byte TimeVal request; ownership does not change the command form.
    {0x0004000cu, "metronome_usec", M2_DEVFORM_MODELED},
    {0x0004000cu, "metronome_bad_signal_mask", M2_DEVFORM_REFUSED},
    {0x0004000cu, "metronome_bad_send_argument", M2_DEVFORM_REFUSED},
    // All four usec commands have source-defined eight-byte TimeVal shapes.
    {0x00040008u, "gettime_usec_valid", M2_DEVFORM_MODELED},
    {0x00040008u, "gettime_usec_bad_recv_argument", M2_DEVFORM_REFUSED},
    {0x00040009u, "settime_usec_valid", M2_DEVFORM_MODELED},
    {0x00040009u, "settime_usec_bad_send_argument", M2_DEVFORM_REFUSED},
    {0x0004000au, "delay_usec_valid", M2_DEVFORM_MODELED},
    {0x0004000au, "delay_usec_bad_send_argument", M2_DEVFORM_REFUSED},
    {0x0004000bu, "delayuntil_usec_valid", M2_DEVFORM_MODELED},
    {0x0004000bu, "delayuntil_usec_bad_send_argument", M2_DEVFORM_REFUSED},
    {0x00040003u, "gettime_vbl_valid", M2_DEVFORM_MODELED},
    {0x00040003u, "gettime_vbl_bad_recv_argument", M2_DEVFORM_REFUSED},
    {0x00040005u, "delay_vbl_valid", M2_DEVFORM_MODELED},
    {0x00040006u, "delayuntil_vbl_valid", M2_DEVFORM_MODELED},
    {0x00040007u, "metronome_vbl", M2_DEVFORM_MODELED},
    {0x00040007u, "metronome_vbl_bad_signal_mask", M2_DEVFORM_REFUSED},
    // MPEGVIDEOCMD_CONTROL: the documented CODECDeviceStatus TagArg list. Every
    // id the vendor header names gets a row, including its VRESERVED* placeholders
    // (the vendor switch's default arm answers BADIOARG for them).
    {0x00070003u, "control_bad_send_argument", M2_DEVFORM_REFUSED},
    {0x00070003u, "control_mps_still_setup", M2_DEVFORM_MODELED},
    {0x00070003u, "control_tag_end", M2_DEVFORM_MODELED},
    {0x00070003u, "control_tag_depth", M2_DEVFORM_MODELED},
    {0x00070003u, "control_tag_dither", M2_DEVFORM_MODELED},
    {0x00070003u, "control_tag_hsize", M2_DEVFORM_MODELED},
    {0x00070003u, "control_tag_keyframes", M2_DEVFORM_REFUSED},
    {0x00070003u, "control_tag_m2mode", M2_DEVFORM_MODELED},
    {0x00070003u, "control_tag_play", M2_DEVFORM_MODELED},
    {0x00070003u, "control_tag_reserved0", M2_DEVFORM_REFUSED},
    {0x00070003u, "control_tag_reserved1", M2_DEVFORM_REFUSED},
    {0x00070003u, "control_tag_reserved2", M2_DEVFORM_REFUSED},
    {0x00070003u, "control_tag_reserved3", M2_DEVFORM_REFUSED},
    {0x00070003u, "control_tag_rgbmode", M2_DEVFORM_REFUSED},
    {0x00070003u, "control_tag_skipframes", M2_DEVFORM_REFUSED},
    {0x00070003u, "control_tag_standard", M2_DEVFORM_MODELED},
    {0x00070003u, "control_tag_unknown", M2_DEVFORM_REFUSED},
    {0x00070003u, "control_tag_vsize", M2_DEVFORM_MODELED},
    {0x00070003u, "control_tag_yuvmode", M2_DEVFORM_REFUSED},
    // MPEGVIDEOCMD_WRITE: the documented payload form, plus the shapes the host
    // cannot accumulate (it must not report success for discarded data).
    {0x00070004u, "write_options_malformed", M2_DEVFORM_REFUSED},
    {0x00070004u, "write_payload_out_of_range", M2_DEVFORM_REFUSED},
    {0x00070004u, "write_payload_overflows_device_buffer", M2_DEVFORM_REFUSED},
    {0x00070004u, "write_still_payload", M2_DEVFORM_MODELED},
    {0x00070004u, "write_stream_payload", M2_DEVFORM_MODELED},
    // MPEGVIDEOCMD_READ: the documented destination is a bitmap Item created
    // with BMTAG_MPEGABLE, and ioi_Send.iob_Len is sizeof(Item).
    {0x00070005u, "read_bitmap_item_destination", M2_DEVFORM_MODELED},
    {0x00070005u, "read_destination_bitmap_unusable", M2_DEVFORM_REFUSED},
    {0x00070005u, "read_destination_not_a_bitmap", M2_DEVFORM_REFUSED},
    {0x00070005u, "read_options_malformed", M2_DEVFORM_REFUSED},
    {0x00070005u, "read_send_len_mismatch", M2_DEVFORM_REFUSED},
    // MPEGVIDEOCMD_GETBUFFERINFO: exact 16-byte stream descriptor and
    // 32-byte result, followed by the vendor CalcBufferInfo calculation.
    {0x00070006u, "getbufferinfo_valid", M2_DEVFORM_MODELED},
    {0x00070006u, "getbufferinfo_bad_io_argument", M2_DEVFORM_REFUSED},
    {0x00070006u, "getbufferinfo_invalid_strip_width", M2_DEVFORM_REFUSED},
    // MPEGVIDEOCMD_ALLOCBUFFERS: the exact stream descriptor is read before
    // the device releases/reallocates its persistent decode buffers. A
    // receive block is optional; only an exactly-sized block is written.
    {0x00070009u, "allocbuffers_valid", M2_DEVFORM_MODELED},
    {0x00070009u, "allocbuffers_bad_io_argument", M2_DEVFORM_REFUSED},
    {0x00070009u, "allocbuffers_invalid_strip_width", M2_DEVFORM_REFUSED},
    // The SET commands carry no send payload. Their receive fields identify
    // the client-owned memory block and the request remains in flight while
    // the MPEG device retains that block.
    {0x00070007u, "setstripbuffer_valid", M2_DEVFORM_MODELED},
    {0x00070007u, "setstripbuffer_null_or_zero", M2_DEVFORM_REFUSED},
    {0x00070007u, "setstripbuffer_out_of_range", M2_DEVFORM_REFUSED},
    {0x00070007u, "setstripbuffer_invalid_alignment_or_size", M2_DEVFORM_REFUSED},
    {0x00070008u, "setreferencebuffer_valid", M2_DEVFORM_MODELED},
    {0x00070008u, "setreferencebuffer_null_or_zero", M2_DEVFORM_REFUSED},
    {0x00070008u, "setreferencebuffer_out_of_range", M2_DEVFORM_REFUSED},
    {0x00070008u, "setreferencebuffer_invalid_alignment", M2_DEVFORM_REFUSED},
    // MP_CMD_DISPATCH: the documented MPPacket/Err shapes.
    {0x01060003u, "mp_packet_malformed", M2_DEVFORM_REFUSED},
    {0x01060003u, "mp_packet_recovered_function", M2_DEVFORM_MODELED},
    {0x01060003u, "mp_packet_unnamed_function", M2_DEVFORM_REFUSED},
};

static const uint32_t g_m2_device_form_count =
    (uint32_t)(sizeof(g_m2_device_forms) / sizeof(g_m2_device_forms[0]));

const m2_device_form_entry* m2_device_form_table(uint32_t* count_out) {
    if (count_out) *count_out = g_m2_device_form_count;
    return g_m2_device_forms;
}

// The documented ioi_CmdOptions validation (`mpVideoDriver.c:560 CheckOptions`).
// A null options pointer is valid; a non-null one must be long aligned,
// readable for sizeof(FMVIOReqOptions) and carry zero Reserved2/Reserved3.
static int fmv_options_malformed(const m2_device_options_view* options) {
    if (!options || !options->present) return 0;
    if (!options->aligned || !options->readable) return 1;
    return options->reserved2 != 0u || options->reserved3 != 0u;
}

// The recovered IOInfo range check every payload form shares.
static int buffer_out_of_range(uint32_t buffer, uint32_t len, uint32_t mem_size) {
    if (len == 0u) return 0;   // the documented zero-length write
    if (buffer == 0u) return 1;
    if (len > mem_size) return 1;
    return buffer > mem_size - len;
}

static int32_t refuse(const char** form_out, const char* form, int32_t status) {
    if (form_out) *form_out = form;
    return status;
}

static int32_t accept(const char** form_out, const char* form) {
    if (form_out) *form_out = form;
    return M2K_OK;
}

static uint32_t mpeg_alloc_round(uint32_t size, uint32_t alignment) {
    return (size + (alignment - 1u)) & ~(alignment - 1u);
}

int32_t m2_mpeg_calc_buffer_info(int32_t fmv_width, int32_t fmv_height,
                                 int32_t still_width,
                                 m2_mpeg_buffer_info* out) {
    if (!out) return (int32_t)M2_DEVCMD_ERR_BAD_IO_ARG;

    uint32_t ref_bytes = 0u;
    uint32_t rounded_fmv_width = 0u;
    if (fmv_width > 0 && fmv_height > 0) {
        rounded_fmv_width = mpeg_alloc_round((uint32_t)fmv_width, 16u);
        const uint32_t rounded_fmv_height =
            mpeg_alloc_round((uint32_t)fmv_height, 16u);
        // The vendor source performs this arithmetic in its uint32_t
        // temporaries. Keep the normal image range exact and reject only a
        // result that cannot be represented in the returned uint32_t field.
        const uint64_t yuv_bytes =
            ((uint64_t)rounded_fmv_width * rounded_fmv_height * 3u) / 2u;
        const uint64_t rounded_ref =
            ((yuv_bytes + M2_DEVCMD_MPEG_BUFFER_PAGE_SIZE - 1u) /
             M2_DEVCMD_MPEG_BUFFER_PAGE_SIZE) *
            M2_DEVCMD_MPEG_BUFFER_PAGE_SIZE * 2u;
        if (rounded_ref > UINT32_MAX) return (int32_t)M2_DEVCMD_ERR_BAD_IO_ARG;
        ref_bytes = (uint32_t)rounded_ref;
    }

    int32_t effective_still_width = still_width;
    if (effective_still_width < (int32_t)rounded_fmv_width)
        effective_still_width = (int32_t)rounded_fmv_width;
    else
        effective_still_width = (int32_t)mpeg_alloc_round(
            (uint32_t)effective_still_width, 16u);

    uint32_t strip_bytes = 0u;
    if (effective_still_width == 0) {
        strip_bytes = 0u;
    } else if (effective_still_width <= 341) {
        strip_bytes = 16u * 1024u;
    } else if (effective_still_width <= 682) {
        strip_bytes = 32u * 1024u;
    } else if (effective_still_width <= 1365) {
        strip_bytes = 64u * 1024u;
    } else if (effective_still_width <= 2730) {
        strip_bytes = 128u * 1024u;
    } else {
        return (int32_t)M2_DEVCMD_MPEG_PARAM_ERROR;
    }

    out->ref_min_size = ref_bytes;
    out->ref_mem_flags = 0u; // MEMTYPE_NORMAL
    out->ref_mem_care_bits = M2_DEVCMD_MPEG_BUFFER_PAGE_SIZE - 1u;
    out->ref_mem_state_bits = 0u;
    out->strip_min_size = strip_bytes;
    out->strip_mem_flags = 0u; // MEMTYPE_NORMAL
    out->strip_mem_care_bits = strip_bytes - 1u;
    out->strip_mem_state_bits = 0u;
    return M2K_OK;
}

static int mpeg_buffer_out_of_range(uint32_t buffer, uint32_t length,
                                    uint32_t mem_size) {
    if (buffer == 0u || length == 0u) return 0;
    return buffer > mem_size || length > mem_size - buffer;
}

int32_t m2_mpeg_set_buffer_form_check(uint32_t command, uint32_t buffer,
                                      uint32_t length, uint32_t mem_size,
                                      const char** form_out) {
    if (form_out) *form_out = "unclassified";
    const int is_strip = command == M2_DEVCMD_MPEGV_SETSTRIPBUFFER;
    const int is_reference = command == M2_DEVCMD_MPEGV_SETREFERENCEBUFFER;
    if (!is_strip && !is_reference)
        return refuse(form_out, "unclassified",
                      (int32_t)M2_DEVCMD_ERR_BAD_COMMAND);

    if (buffer == 0u || length == 0u)
        return refuse(form_out,
                      is_strip ? "setstripbuffer_null_or_zero"
                               : "setreferencebuffer_null_or_zero",
                      (int32_t)M2_DEVCMD_MPEG_BAD_PTR);
    if (mpeg_buffer_out_of_range(buffer, length, mem_size))
        return refuse(form_out,
                      is_strip ? "setstripbuffer_out_of_range"
                               : "setreferencebuffer_out_of_range",
                      M2K_ERR_RANGE);

    if (is_strip) {
        const int valid_size = length == 16u * 1024u ||
                               length == 32u * 1024u ||
                               length == 64u * 1024u ||
                               length == 128u * 1024u;
        if (!valid_size || (buffer & (length - 1u)) != 0u)
            return refuse(form_out, "setstripbuffer_invalid_alignment_or_size",
                          (int32_t)M2_DEVCMD_MPEG_PARAM_ERROR);
        return accept(form_out, "setstripbuffer_valid");
    }

    if ((buffer & (M2_DEVCMD_MPEG_BUFFER_PAGE_SIZE - 1u)) != 0u ||
        (length & (M2_DEVCMD_MPEG_BUFFER_PAGE_SIZE - 1u)) != 0u)
        return refuse(form_out, "setreferencebuffer_invalid_alignment",
                      (int32_t)M2_DEVCMD_MPEG_PARAM_ERROR);
    return accept(form_out, "setreferencebuffer_valid");
}

// The lifted MPS_InitMPEGStill path sends one complete, source-authored
// CODECDeviceStatus list at 0x2038c. Its values select the host decoder's
// existing fixed still contract: square/M2 mode, 16-bit RGB555, 576x432, and
// no keyframe-only filtering. Match the complete list and caller site rather
// than accepting any individual unsupported tag as a silent no-op.
static int mps_still_control_setup(const m2_device_control_view* view) {
    static const uint32_t expected_tag[] = {
        M2_DEVCTL_TAG_STANDARD,
        M2_DEVCTL_TAG_DEPTH,
        M2_DEVCTL_TAG_HSIZE,
        M2_DEVCTL_TAG_VSIZE,
        M2_DEVCTL_TAG_M2MODE,
        M2_DEVCTL_TAG_KEYFRAMES,
        M2_DEVCTL_TAG_END,
    };
    static const uint32_t expected_arg[] = {
        M2_DEVCTL_MPS_STILL_STANDARD,
        M2_DEVCTL_MPS_STILL_DEPTH,
        M2_DEVCTL_MPS_STILL_HSIZE,
        M2_DEVCTL_MPS_STILL_VSIZE,
        M2_DEVCTL_MPS_STILL_M2MODE,
        M2_DEVCTL_MPS_STILL_KEYFRAMES,
        0u,
    };
    if (!view || view->selector_pc != M2_DEVCTL_MPS_STILL_CONTROL_PC ||
        view->tag_count != sizeof(expected_tag) / sizeof(expected_tag[0]))
        return 0;
    for (uint32_t i = 0; i < view->tag_count; ++i) {
        if (view->tag[i] != expected_tag[i] ||
            view->tag_arg[i] != expected_arg[i])
            return 0;
    }
    return 1;
}

int32_t m2_device_argument_form_check(const m2_device_argument_view* view,
                                      const m2_device_options_view* options,
                                      const char** form_out) {
    if (form_out) *form_out = "unclassified";
    if (!view) return M2_DEVCMD_ERR_BAD_IO_ARG;

    switch (view->command) {
    case M2_DEVCMD_CMD_READ:
        // The host file pool serves CMD_STATUS/CMD_BLOCKREAD by block; a
        // generic read of it is refused rather than completed with nothing.
        if (view->device_is_file)
            return refuse(form_out, "read_file_device", M2K_ERR_BAD_ITEM);
        if (view->recv_len == M2_DEVCMD_FMV_STILL_BYTES ||
            view->selector_pc == M2_DEVCMD_FMV_SUBSCRIBER_PC)
            return accept(form_out, "read_fmv_still_frame");
        return refuse(form_out, "read_other_device",
                      (int32_t)M2_DEVCMD_ERR_NOT_SUPPORTED);

    case M2_DEVCMD_CMD_STATUS:
        if (view->device_is_file) {
            if (view->recv_len < M2_DEVCMD_STATUS_MAX_LEN)
                return accept(form_out, "status_file_device_truncated_record");
            return accept(form_out, "status_file_device_record");
        }
        if (view->recv_len < M2_DEVCMD_STATUS_MIN_LEN)
            return refuse(form_out, "status_nonfile_device_short_record",
                          (int32_t)M2_DEVCMD_ERR_BAD_IO_ARG);
        if (buffer_out_of_range(view->recv_buffer, view->recv_len,
                                view->mem_size))
            return refuse(form_out, "status_nonfile_device_out_of_range",
                          M2K_ERR_RANGE);
        return accept(form_out, "status_nonfile_device_record");

    case M2_DEVCMD_CMD_BLOCKREAD:
        if (!view->device_is_file)
            return refuse(form_out, "blockread_nonfile_device",
                          (int32_t)M2_DEVCMD_ERR_NOT_SUPPORTED);
        if (view->recv_len % 2048u != 0u)
            return refuse(form_out, "blockread_file_partial_block_length",
                          M2K_ERR_RANGE);
        return accept(form_out, "blockread_file_block_multiple");

    case M2_DEVCMD_TIMER_GETTIME_VBL:
        // CmdGetTimeVBL accepts a receive block at least as large as one
        // TimeValVBL and writes exactly the eight-byte VBL counter.
        if (view->recv_len < M2_DEVCMD_TIMER_VBL_COUNT_SIZE ||
            buffer_out_of_range(view->recv_buffer,
                                M2_DEVCMD_TIMER_VBL_COUNT_SIZE,
                                view->mem_size))
            return refuse(form_out, "gettime_vbl_bad_recv_argument",
                          (int32_t)M2_DEVCMD_ERR_BAD_IO_ARG);
        return accept(form_out, "gettime_vbl_valid");

    case M2_DEVCMD_TIMER_DELAY_VBL:
        // CmdDelayVBL has no buffer validation: ioi_Offset is the unsigned
        // countdown consumed by VBLHandler.
        return accept(form_out, "delay_vbl_valid");

    case M2_DEVCMD_TIMER_DELAYUNTIL_VBL:
        // CmdDelayUntilVBL has the same source shape; the driver subtracts the
        // current low VBL word before queueing the request.
        return accept(form_out, "delayuntil_vbl_valid");

    case M2_DEVCMD_TIMER_METRONOME_VBL:
        // CmdMetronomeVBL validates only that the signal mask uses bits 8-30.
        if ((view->cmd_options & 0x800000ffu) != 0u)
            return refuse(form_out, "metronome_vbl_bad_signal_mask",
                          (int32_t)M2_DEVCMD_ERR_BAD_IO_ARG);
        return accept(form_out, "metronome_vbl");

    case M2_DEVCMD_TIMER_GETTIME_USEC:
        // CmdGetTimeUSec accepts a receive block at least as large as one
        // TimeVal and writes exactly that structure. Keep the host range
        // check to the bytes the released driver actually stores.
        if (view->recv_len < M2_DEVCMD_TIMER_TIMEVAL_SIZE ||
            buffer_out_of_range(view->recv_buffer,
                                M2_DEVCMD_TIMER_TIMEVAL_SIZE,
                                view->mem_size))
            return refuse(form_out, "gettime_usec_bad_recv_argument",
                          (int32_t)M2_DEVCMD_ERR_BAD_IO_ARG);
        return accept(form_out, "gettime_usec_valid");

    case M2_DEVCMD_TIMER_SETTIME_USEC:
        // CmdSetTimeUSec requires exactly one readable TimeVal and preserves
        // the vendor's one-sided tv_Microseconds <= 1000000 check (negative
        // values are not rejected by the released source).
        if (view->send_len != M2_DEVCMD_TIMER_TIMEVAL_SIZE ||
            buffer_out_of_range(view->send_buffer,
                                M2_DEVCMD_TIMER_TIMEVAL_SIZE,
                                view->mem_size) ||
            view->timer_microseconds > 1000000)
            return refuse(form_out, "settime_usec_bad_send_argument",
                          (int32_t)M2_DEVCMD_ERR_BAD_IO_ARG);
        return accept(form_out, "settime_usec_valid");

    case M2_DEVCMD_TIMER_DELAY_USEC:
        // CmdDelayUSec consumes exactly one TimeVal and only rejects a
        // microsecond field above one second; negative fields are accepted by
        // the released unsigned conversion and therefore remain source-valid.
        if (view->send_len != M2_DEVCMD_TIMER_TIMEVAL_SIZE ||
            buffer_out_of_range(view->send_buffer,
                                M2_DEVCMD_TIMER_TIMEVAL_SIZE,
                                view->mem_size) ||
            view->timer_microseconds > 1000000)
            return refuse(form_out, "delay_usec_bad_send_argument",
                          (int32_t)M2_DEVCMD_ERR_BAD_IO_ARG);
        return accept(form_out, "delay_usec_valid");

    case M2_DEVCMD_TIMER_DELAYUNTIL_USEC:
        if (view->send_len != M2_DEVCMD_TIMER_TIMEVAL_SIZE ||
            buffer_out_of_range(view->send_buffer,
                                M2_DEVCMD_TIMER_TIMEVAL_SIZE,
                                view->mem_size) ||
            view->timer_microseconds > 1000000)
            return refuse(form_out, "delayuntil_usec_bad_send_argument",
                          (int32_t)M2_DEVCMD_ERR_BAD_IO_ARG);
        return accept(form_out, "delayuntil_usec_valid");

    case M2_DEVCMD_TIMER_METRONOME_USEC:
        if ((view->cmd_options & 0x800000ffu) != 0u)
            return refuse(form_out, "metronome_bad_signal_mask",
                          (int32_t)M2_DEVCMD_ERR_BAD_IO_ARG);
        if (view->send_len != 8u ||
            buffer_out_of_range(view->send_buffer, view->send_len,
                                view->mem_size))
            return refuse(form_out, "metronome_bad_send_argument",
                          (int32_t)M2_DEVCMD_ERR_BAD_IO_ARG);
        return accept(form_out, "metronome_usec");

    case 0x00070004u: // MPEGVIDEOCMD_WRITE
        if (fmv_options_malformed(options))
            return refuse(form_out, "write_options_malformed",
                          (int32_t)M2_DEVCMD_ERR_BAD_IO_ARG);
        if (buffer_out_of_range(view->send_buffer, view->send_len,
                                view->mem_size))
            return refuse(form_out, "write_payload_out_of_range",
                          M2K_ERR_RANGE);
        if (view->device_buffer_capacity != 0u &&
            (view->device_buffer_used >= view->device_buffer_capacity ||
             view->send_len > view->device_buffer_capacity -
                                view->device_buffer_used))
            return refuse(form_out, "write_payload_overflows_device_buffer",
                          M2K_ERR_RANGE);
        return accept(form_out, view->device_buffer_capacity != 0u
                                    ? "write_still_payload"
                                    : "write_stream_payload");

    case 0x00070005u: // MPEGVIDEOCMD_READ
        if (fmv_options_malformed(options))
            return refuse(form_out, "read_options_malformed",
                          (int32_t)M2_DEVCMD_ERR_BAD_IO_ARG);
        // The documented send shape is a bitmap Item: iob_Len = sizeof(Item)
        // and iob_Buffer pointing at it.
        if (view->send_len != 4u || view->send_buffer == 0u ||
            buffer_out_of_range(view->send_buffer, 4u, view->mem_size))
            return refuse(form_out, "read_send_len_mismatch",
                          (int32_t)M2_DEVCMD_ERR_BAD_IO_ARG);
        if (!view->dest_is_bitmap)
            return refuse(form_out, "read_destination_not_a_bitmap",
                          M2K_ERR_BAD_ITEM);
        if (!view->dest_bitmap_live)
            return refuse(form_out, "read_destination_bitmap_unusable",
                          M2K_ERR_BAD_ITEM);
        return accept(form_out, "read_bitmap_item_destination");

    case M2_DEVCMD_MPEGV_GETBUFFERINFO: {
        if (view->send_len != M2_DEVCMD_MPEG_STREAM_INFO_SIZE ||
            view->recv_len != M2_DEVCMD_MPEG_BUFFER_INFO_SIZE ||
            buffer_out_of_range(view->send_buffer, view->send_len,
                                view->mem_size) ||
            buffer_out_of_range(view->recv_buffer, view->recv_len,
                                view->mem_size))
            return refuse(form_out, "getbufferinfo_bad_io_argument",
                          (int32_t)M2_DEVCMD_ERR_BAD_IO_ARG);
        m2_mpeg_buffer_info info;
        if (m2_mpeg_calc_buffer_info(view->mpeg_fmv_width,
                                     view->mpeg_fmv_height,
                                     view->mpeg_still_width, &info) < 0)
            return refuse(form_out, "getbufferinfo_invalid_strip_width",
                          (int32_t)M2_DEVCMD_MPEG_PARAM_ERROR);
        return accept(form_out, "getbufferinfo_valid");
    }

    case M2_DEVCMD_MPEGV_ALLOCBUFFERS: {
        // mpvCmdAllocBuffers requires a readable MPEGBufferStreamInfo. The
        // fourth word (stillHeight) is carried by the ABI but is not consumed
        // by CalcBufferInfo, exactly as in the released driver.
        if (view->send_len != M2_DEVCMD_MPEG_STREAM_INFO_SIZE ||
            buffer_out_of_range(view->send_buffer, view->send_len,
                                view->mem_size))
            return refuse(form_out, "allocbuffers_bad_io_argument",
                          (int32_t)M2_DEVCMD_ERR_BAD_IO_ARG);
        // The driver only copies an exactly-sized optional result. When that
        // form is requested, the kernel's writable-memory validation is
        // represented by the same guest-range check used by GETBUFFERINFO.
        if (view->recv_len == M2_DEVCMD_MPEG_BUFFER_INFO_SIZE &&
            buffer_out_of_range(view->recv_buffer, view->recv_len,
                                view->mem_size))
            return refuse(form_out, "allocbuffers_bad_io_argument",
                          (int32_t)M2_DEVCMD_ERR_BAD_IO_ARG);
        m2_mpeg_buffer_info info;
        if (m2_mpeg_calc_buffer_info(view->mpeg_fmv_width,
                                     view->mpeg_fmv_height,
                                     view->mpeg_still_width, &info) < 0)
            return refuse(form_out, "allocbuffers_invalid_strip_width",
                          (int32_t)M2_DEVCMD_MPEG_PARAM_ERROR);
        return accept(form_out, "allocbuffers_valid");
    }

    case M2_DEVCMD_MPEGV_SETSTRIPBUFFER:
    case M2_DEVCMD_MPEGV_SETREFERENCEBUFFER:
        return m2_mpeg_set_buffer_form_check(
            view->command, view->recv_buffer, view->recv_len,
            view->mem_size, form_out);

    case 0x01060003u: { // MP_CMD_DISPATCH
        // Documented shape: ioi_Send is an MPPacket (20 bytes), ioi_Recv is an
        // Err (4 bytes).
        if (view->send_len != 20u || view->recv_len != 4u ||
            buffer_out_of_range(view->send_buffer, 20u, view->mem_size) ||
            buffer_out_of_range(view->recv_buffer, 4u, view->mem_size))
            return refuse(form_out, "mp_packet_malformed",
                          (int32_t)M2_DEVCMD_ERR_BAD_IO_ARG);
        // dest_is_bitmap is reused as "the packet names a function the host can
        // run" by the MP bridge, which is the only slave entry the host has.
        if (!view->dest_is_bitmap)
            return refuse(form_out, "mp_packet_unnamed_function",
                          (int32_t)M2_DEVCMD_ERR_NOT_SUPPORTED);
        return accept(form_out, "mp_packet_recovered_function");
    }

    default:
        break;
    }
    return M2K_OK;   // not an accepted command with argument forms
}

int32_t m2_device_control_form_check(const m2_device_control_view* view,
                                     const char** form_out) {
    if (form_out) *form_out = "unclassified";
    if (!view) return M2_DEVCMD_ERR_BAD_IO_ARG;

    // The vendor driver rejects a null, unaligned or unreadable
    // CODECDeviceStatus before it reads a single tag.
    if (view->send_buffer == 0u || !view->buffer_aligned ||
        view->send_len < M2_DEVCTL_CODEC_STATUS_SIZE ||
        view->send_buffer > view->mem_size ||
        view->send_len > view->mem_size - view->send_buffer)
        return refuse(form_out, "control_bad_send_argument",
                      (int32_t)M2_DEVCMD_ERR_BAD_IO_ARG);

    if (mps_still_control_setup(view))
        return accept(form_out, "control_mps_still_setup");

    // The vendor loop stops at TAG_END, so an immediately terminated list asks
    // for nothing; a longer list reports the last documented tag it named.
    const char* last = "control_tag_end";
    for (uint32_t i = 0; i < view->tag_count; ++i) {
        const char* form = NULL;
        int32_t status = M2K_OK;
        switch (view->tag[i]) {
        case M2_DEVCTL_TAG_END:
            return accept(form_out, last);
        case M2_DEVCTL_TAG_DITHER:
            // "WARNING: VID_CODEC_TAG_DITHER ignored" -- the vendor driver
            // itself fixes this tag's behaviour, so mirroring it is faithful.
            form = "control_tag_dither";
            break;
        case M2_DEVCTL_TAG_HSIZE:
            form = "control_tag_hsize";
            break;
        case M2_DEVCTL_TAG_VSIZE:
            form = "control_tag_vsize";
            break;
        case M2_DEVCTL_TAG_M2MODE:
            // The host decoder writes M2-format pixels for M2 titles.
            form = "control_tag_m2mode";
            break;
        case M2_DEVCTL_TAG_PLAY:
            // The host decoder returns every picture in the stream.
            form = "control_tag_play";
            break;
        case M2_DEVCTL_TAG_DEPTH: {
            form = "control_tag_depth";
            // Portfolio mpVideoDriver.c:678 and M2MPEGUnit.c:209 accept 16 or
            // 24-bit pixel depth (or obsolete negative depth).
            int32_t depth = (int32_t)view->tag_arg[i];
            if (depth < 0) depth = -depth;
            if (depth != 16 && depth != 24)
                status = (int32_t)M2_DEVCMD_ERR_BAD_IO_ARG;
            break;
        }
        case M2_DEVCTL_TAG_STANDARD:
            form = "control_tag_standard";
            // Portfolio mpVideoDriver.c:705 and M2MPEGUnit.c:242 accept standard
            // square resampling (kCODEC_SQUARE_RESAMPLE = 0). Non-zero resample
            // modes are unsupported on M2 hardware and return BADIOARG.
            if (view->tag_arg[i] != 0u)
                status = (int32_t)M2_DEVCMD_ERR_BAD_IO_ARG;
            break;
        case M2_DEVCTL_TAG_SKIPFRAMES:
            form = "control_tag_skipframes";
            status = (int32_t)M2_DEVCMD_ERR_NOT_SUPPORTED;
            break;
        case M2_DEVCTL_TAG_KEYFRAMES:
            form = "control_tag_keyframes";
            status = (int32_t)M2_DEVCMD_ERR_NOT_SUPPORTED;
            break;
        case M2_DEVCTL_TAG_YUVMODE:
            form = "control_tag_yuvmode";
            status = (int32_t)M2_DEVCMD_ERR_NOT_SUPPORTED;
            break;
        case M2_DEVCTL_TAG_RGBMODE:
            form = "control_tag_rgbmode";
            status = (int32_t)M2_DEVCMD_ERR_NOT_SUPPORTED;
            break;
        case M2_DEVCTL_TAG_RESERVED0:
            form = "control_tag_reserved0";
            status = (int32_t)M2_DEVCMD_ERR_BAD_IO_ARG;
            break;
        case M2_DEVCTL_TAG_RESERVED1:
            form = "control_tag_reserved1";
            status = (int32_t)M2_DEVCMD_ERR_BAD_IO_ARG;
            break;
        case M2_DEVCTL_TAG_RESERVED2:
            form = "control_tag_reserved2";
            status = (int32_t)M2_DEVCMD_ERR_BAD_IO_ARG;
            break;
        case M2_DEVCTL_TAG_RESERVED3:
            form = "control_tag_reserved3";
            status = (int32_t)M2_DEVCMD_ERR_BAD_IO_ARG;
            break;
        default:
            form = "control_tag_unknown";
            status = (int32_t)M2_DEVCMD_ERR_BAD_IO_ARG;
            break;
        }
        if (status < 0) return refuse(form_out, form, status);
        last = form;
    }
    // Every tag in the list is one the host's decoder configuration already
    // implements, so the request is a no-op it can honour.
    return accept(form_out, last);
}

void m2_device_status_record_write(uint8_t* dst, uint32_t len,
                                   uint32_t* actual_out) {
    if (actual_out) *actual_out = 0u;
    if (!dst) return;
    uint32_t written = len;
    if (written > M2_DEVCMD_STATUS_SIZE) written = M2_DEVCMD_STATUS_SIZE;
    for (uint32_t i = 0; i < written; ++i) dst[i] = 0u;
    // DeviceStatus (`kernel/driver.h:30`): 4 bytes of
    // identity/version/family/reserved, then ds_MaximumStatusSize at +0x04 and
    // ds_DeviceBlockSize at +0x08. A zero block size is the documented "not a
    // block-oriented device" (`devicecmd.h CMD_BLOCKREAD`). The identity,
    // version and family bytes are device-specific values the host does not
    // have; they stay zero (docs/recompilation/limitations.md).
    if (written >= 0x08u) {
        // Guest fields are big-endian (PowerPC), as the host's wr32 stores them.
        dst[0x04u] = (uint8_t)((M2_DEVCMD_STATUS_SIZE >> 24) & 0xffu);
        dst[0x05u] = (uint8_t)((M2_DEVCMD_STATUS_SIZE >> 16) & 0xffu);
        dst[0x06u] = (uint8_t)((M2_DEVCMD_STATUS_SIZE >> 8) & 0xffu);
        dst[0x07u] = (uint8_t)(M2_DEVCMD_STATUS_SIZE & 0xffu);
    }
    if (actual_out) *actual_out = written;
}

static void status_be32(uint8_t* dst, uint32_t offset, uint32_t value) {
    dst[offset + 0u] = (uint8_t)(value >> 24);
    dst[offset + 1u] = (uint8_t)(value >> 16);
    dst[offset + 2u] = (uint8_t)(value >> 8);
    dst[offset + 3u] = (uint8_t)value;
}

void m2_file_status_record_write(uint8_t* dst, uint32_t len,
                                 uint32_t file_bytes, uint32_t block_size,
                                 uint32_t* actual_out) {
    if (actual_out) *actual_out = 0u;
    if (!dst && len != 0u) return;
    uint32_t written = len;
    if (written > M2_DEVCMD_STATUS_MAX_LEN)
        written = M2_DEVCMD_STATUS_MAX_LEN;
    if (written != 0u)
        memset(dst, 0, written);

    // FileDriverStatus (folios/file/driver.c): DI_FILE, DS_DEVTYPE_FILE,
    // sizeof(FileStatus), the opened file geometry, and read-only flags for
    // the production disc view. Each store is prefix-safe because the source
    // explicitly permits a truncated receive record.
    if (written >= 1u) dst[0x00u] = M2_DEVCMD_FILE_DRIVER_IDENTITY;
    if (written >= 3u) dst[0x02u] = M2_DEVCMD_FILE_FAMILY_CODE;
    if (written >= 8u)
        status_be32(dst, 0x04u, M2_DEVCMD_STATUS_MAX_LEN);
    if (written >= 12u)
        status_be32(dst, 0x08u, block_size);
    if (written >= 16u) {
        const uint32_t blocks = block_size == 0u
            ? 0u : (file_bytes + block_size - 1u) / block_size;
        status_be32(dst, 0x0cu, blocks);
    }
    if (written >= 24u)
        status_be32(dst, 0x14u, M2_DEVCMD_FILE_READONLY_FLAG);
    if (written >= 28u)
        status_be32(dst, 0x18u, M2_DEVCMD_USAGE_READONLY);
    if (written >= 44u)
        status_be32(dst, 0x28u, file_bytes);
    if (written >= 52u)
        status_be32(dst, 0x30u, M2_DEVCMD_FILE_READONLY_FLAG);
    if (actual_out) *actual_out = written;
}
