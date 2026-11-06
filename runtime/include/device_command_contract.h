// Device-command accepted set for the guest-visible I/O boundary.
//
// The vocabulary for this interface is the documented M2 command table
// : every device class and every
// `MakeDeviceCmd(class, n)` word the M2 interface documents.
// the command-contract audit and tests pin the decisions.
//
// The invariant this exists to enforce is the same one the CLT register file and
// the Triangle Engine command surface already carry: a documented request form
// is either implemented and tested, or refused with a named identity — never
// silently completed. The host's generic send path (`m2k_send_io`) is
// command-agnostic and completes *any* request with `M2K_OK`, so a documented
// command the host does not model would otherwise report success to the guest
// while leaving the response buffers the command documents untouched.
//
// The refusal identities are the M2 interface's own standard errors, not host
// inventions. The M2 error interface defines
//     #define NOSUPPORT MakeKErr(ER_SEVERE, ER_C_STND, ER_NotSupported)
//     #define BADCOMMAND MakeKErr(ER_SEVERE, ER_C_STND, ER_BadCommand)
// with `ER_NotSupported = 16` ("Function not supported in this rev") and
// `ER_BadCommand = 12` ("Bad Command field in IOReq"). The MAKEERR layout they
// are built from reproduces the project's `GK_GFX_ERR_BADITEM` (0xd55b9001)
// exactly, so these values are derived rather than guessed.
#ifndef M2_DEVICE_COMMAND_CONTRACT_H
#define M2_DEVICE_COMMAND_CONTRACT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// MakeDeviceCmd(class, cmd) == ((class) << 16) | cmd  (devicecmd.h).
#define M2_DEVICE_CMD(class, number) ((((uint32_t)(class)) << 16) | (number))
#define M2_DEVICE_CMD_CLASS(command) ((uint32_t)(command) >> 16)
#define M2_DEVICE_CMD_NUMBER(command) ((uint32_t)(command) & 0xffffu)

// Documented command classes (devicecmd.h).
#define M2_DEVCLASS_GENERIC     0x000u
#define M2_DEVCLASS_GFX         0x002u
#define M2_DEVCLASS_FILE        0x003u
#define M2_DEVCLASS_TIMER       0x004u
#define M2_DEVCLASS_CTLPORT     0x005u
#define M2_DEVCLASS_DISC        0x006u
#define M2_DEVCLASS_MPEGV       0x007u
#define M2_DEVCLASS_PROXY       0x009u
#define M2_DEVCLASS_USLOT       0x00au
#define M2_DEVCLASS_SLOT        0x00bu
#define M2_DEVCLASS_SERIAL      0x00cu
#define M2_DEVCLASS_TE          0x00du
#define M2_DEVCLASS_HOST        0x103u
#define M2_DEVCLASS_HOSTFS      0x104u
#define M2_DEVCLASS_HOSTCONSOLE 0x105u
#define M2_DEVCLASS_MP          0x106u

// What the host does with one documented command word, or with a word the
// documented vocabulary does not contain at all.
typedef enum m2_device_command_disposition {
    // The host models this command (directly, or through the typed boundary
    // that owns it: the TE command contract, the MP device bridge, the timer
    // metronome bridge, the file/FMV read paths).
    M2_DEVCMD_HANDLED = 0,
    // Documented by the M2 interface, but this host has no device model for it. The
    // request is completed with the M2 interface's ER_NotSupported identity instead of
    // the command-agnostic success the shared send path would report.
    M2_DEVCMD_UNMODELLED = 1,
    // Not part of the documented vocabulary at all. Completed with the M2 interface's
    // ER_BadCommand identity.
    M2_DEVCMD_UNDOCUMENTED = 2,
} m2_device_command_disposition;

// Look up one command word. Never fails: an unknown word is UNDOCUMENTED.
m2_device_command_disposition m2_device_command_lookup(uint32_t command);

// The documented name of a command word ("CDROMCMD_READ"), or "" when the word
// is not in the documented vocabulary. Used by the audit table and the
// diagnostic trace; never by guest-visible behavior.
const char* m2_device_command_name(uint32_t command);

// Every documented command word the table enumerates, for the audit. The count
// is fixed at build time so the scanner and the focused test can assert that
// the host table still covers the whole M2 vocabulary.
typedef struct m2_device_command_entry {
    uint32_t command;
    const char* name;
    m2_device_command_disposition disposition;
} m2_device_command_entry;

const m2_device_command_entry* m2_device_command_table(uint32_t* count_out);

// Error identities (operror.h): MakeKErr(ER_SEVERE, ER_C_STND, err).
#define M2_DEVCMD_ERR_NOT_SUPPORTED 0xd57b9010u // ER_NotSupported = 16
#define M2_DEVCMD_ERR_BAD_COMMAND   0xd57b900cu // ER_BadCommand = 12
#define M2_DEVCMD_ERR_BAD_IO_ARG    0xd57b900du // ER_BadIOArg = 13

// ---- documented argument forms --------------------------------------------
//
// A command word is one dimension of the accepted set; the documented
// *argument form* is another. Every M2 driver validates more than the
// word, and every one of those validations is a documented refusal the host has
// to reproduce instead of completing the request with the command-agnostic
// success `m2k_send_io` would report:
//
//   * `mpegvideo/mpVideoDriver.c:560 CheckOptions` rejects a non-null
//     `ioi_CmdOptions` that is not long aligned, is not readable for
//     `sizeof(FMVIOReqOptions)` (24 bytes), or carries non-zero
//     `Reserved2`/`Reserved3` (offsets 16/20); `mpvCmdRead`/`mpvCmdWrite`
//     answer `BADIOARG`.
//   * `mpegvideo/mpVideoDriver.c:624 mpvCmdControl` rejects a null, unaligned or
//     unreadable `CODECDeviceStatus`, and answers `BADIOARG` for any TagArg id
//     its switch does not name (including the header's `VRESERVED*` ids).
//   * `mpegvideo/mpVideoDriver.c:772 mpvCmdStatus` answers `BADIOARG` when
//     `ioi_Recv.iob_Len < 8`, else writes the documented `DeviceStatus`
//     skeleton and sets `io_Actual`.
//
// The form table below is the host's declared set for those cases; the
// device-command vocabulary rows the same forms; keep the two sets equal so a
// form is never added to one side only.
typedef enum m2_device_form_disposition {
    // The host models this documented form; the request completes normally.
    M2_DEVFORM_MODELED = 0,
    // Documented, but the host refuses it with a named identity instead of
    // reporting a success it cannot honour.
    M2_DEVFORM_REFUSED = 1,
} m2_device_form_disposition;

typedef struct m2_device_form_entry {
    uint32_t command;
    const char* form;
    m2_device_form_disposition disposition;
} m2_device_form_entry;

const m2_device_form_entry* m2_device_form_table(uint32_t* count_out);

// Documented generic-command argument forms whose shape the host decides.
#define M2_DEVCMD_CMD_READ      0x00000001u
#define M2_DEVCMD_CMD_STATUS    0x00000002u
#define M2_DEVCMD_CMD_BLOCKREAD 0x000000c3u
#define M2_DEVCMD_TIMER_GETTIME_USEC    0x00040008u
#define M2_DEVCMD_TIMER_SETTIME_USEC    0x00040009u
#define M2_DEVCMD_TIMER_DELAY_USEC      0x0004000au
#define M2_DEVCMD_TIMER_DELAYUNTIL_USEC 0x0004000bu
#define M2_DEVCMD_TIMER_METRONOME_USEC 0x0004000cu
#define M2_DEVCMD_TIMER_GETTIME_VBL     0x00040003u
#define M2_DEVCMD_TIMER_DELAY_VBL       0x00040005u
#define M2_DEVCMD_TIMER_DELAYUNTIL_VBL  0x00040006u
#define M2_DEVCMD_TIMER_METRONOME_VBL   0x00040007u
#define M2_DEVCMD_MPEGV_CONTROL 0x00070003u
#define M2_DEVCMD_MPEGV_WRITE   0x00070004u
#define M2_DEVCMD_MPEGV_READ    0x00070005u
#define M2_DEVCMD_MPEGV_GETBUFFERINFO 0x00070006u
#define M2_DEVCMD_MPEGV_SETSTRIPBUFFER 0x00070007u
#define M2_DEVCMD_MPEGV_SETREFERENCEBUFFER 0x00070008u
#define M2_DEVCMD_MPEGV_ALLOCBUFFERS 0x00070009u
#define M2_DEVCMD_MP_DISPATCH   0x01060003u

// `mpvCmdStatus`'s documented DeviceStatus contract (the M2 driver status contract):
// 4 bytes of identity/version/family/reserved then nine uint32 fields = 40
// bytes; a receive length below 8 is BADIOARG and a longer buffer is clamped.
#define M2_DEVCMD_STATUS_SIZE    40u
#define M2_DEVCMD_STATUS_MIN_LEN 8u
#define M2_DEVCMD_STATUS_MAX_LEN 0x44u // host file pool: FileStatus record
#define M2_DEVCMD_FILE_DRIVER_IDENTITY 0x04u
#define M2_DEVCMD_FILE_FAMILY_CODE     0x03u
#define M2_DEVCMD_FILE_READONLY_FLAG   0x00000002u
#define M2_DEVCMD_USAGE_READONLY       0x20000000u

// `mpvCmdWrite`/`mpvCmdRead`'s documented `ioi_CmdOptions` contract.
#define M2_DEVCMD_FMV_OPTIONS_SIZE            24u
#define M2_DEVCMD_FMV_OPTIONS_RESERVED2_OFFSET 16u
#define M2_DEVCMD_FMV_OPTIONS_RESERVED3_OFFSET 20u

// TimerDriver's usec commands use the M2 kernel TimeVal layout.
#define M2_DEVCMD_TIMER_TIMEVAL_SIZE 8u
#define M2_DEVCMD_TIMER_VBL_COUNT_SIZE 8u

// MPEG buffer negotiation (`device/mpegvideo.h:174-191` and
// `MPEGVideoBuffers.c:59`). The M2 SYSMEM channel publishes a 4K page size;
// both the reference allocation and its address mask depend on that value.
#define M2_DEVCMD_MPEG_BUFFER_PAGE_SIZE 4096u
#define M2_DEVCMD_MPEG_STREAM_INFO_SIZE 16u
#define M2_DEVCMD_MPEG_BUFFER_INFO_SIZE 32u
#define M2_DEVCMD_MPEG_BUFFER_INFO_WORDS 8u
#define M2_DEVCMD_MPEG_BAD_PTR 0xd57b9009u
#define M2_DEVCMD_MPEG_ABORTED 0xd57b900au
#define M2_DEVCMD_MPEG_NOMEM 0xd57b9006u
#define M2_DEVCMD_MPEG_PARAM_ERROR 0xd57b9017u

typedef struct m2_mpeg_buffer_info {
    uint32_t ref_min_size;
    uint32_t ref_mem_flags;
    uint32_t ref_mem_care_bits;
    uint32_t ref_mem_state_bits;
    uint32_t strip_min_size;
    uint32_t strip_mem_flags;
    uint32_t strip_mem_care_bits;
    uint32_t strip_mem_state_bits;
} m2_mpeg_buffer_info;

// Calculate the exact result of the M2 `CalcBufferInfo` routine. The
// stream fields are the original signed int32 inputs; the output words are
// host integers ready to be written through the guest's big-endian helpers.
int32_t m2_mpeg_calc_buffer_info(int32_t fmv_width, int32_t fmv_height,
                                 int32_t still_width,
                                 m2_mpeg_buffer_info* out);

// Validate one client-supplied MPEG buffer using the M2
// ValidateSBAddrSize/UseReferenceBuffers rules. `form_out` names the exact
// form for the device-command vocabulary and may be NULL.
int32_t m2_mpeg_set_buffer_form_check(uint32_t command, uint32_t buffer,
                                      uint32_t length, uint32_t mem_size,
                                      const char** form_out);

// The FMV library's subscriber read: `CMD_READ` reaches the same device handler
// as `MPEGVIDEOCMD_READ` (`mpVideoDriver.c:80 mpvOldCmdRead` calls `mpvCmdRead`),
// but the subscriber form receives a whole decoded 576x432 RGB555 frame in the
// receive buffer, which is the shape the host models. 576 * 432 * 2 = 497664.
#define M2_DEVCMD_FMV_STILL_BYTES   497664u
#define M2_DEVCMD_FMV_SUBSCRIBER_PC 0x000655bcu

// The recovered IOInfo fields for one accepted command, plus the host facts a
// documented form depends on. Every field is either a raw IOInfo field or a
// statement about the host's own device model; none selects a game action.
typedef struct m2_device_argument_view {
    uint32_t command;
    uint32_t cmd_options;   // ioi_CmdOptions
    uint32_t offset;        // ioi_Offset
    uint32_t send_buffer;   // ioi_Send.iob_Buffer
    uint32_t send_len;      // ioi_Send.iob_Len
    uint32_t recv_buffer;   // ioi_Recv.iob_Buffer
    uint32_t recv_len;      // ioi_Recv.iob_Len
    uint32_t mem_size;      // guest memory size, for the documented range checks
    uint32_t selector_pc;   // recovered site (the FMV subscriber-read form)
    // MPEGV still-path accumulator state. Zero capacity means the command has no
    // still destination armed, which is the documented streaming form.
    uint32_t device_buffer_used;
    uint32_t device_buffer_capacity;
    int device_is_file;     // the IOReq's device resolves to the host file pool
    int dest_is_bitmap;     // the armed MPEGV destination resolves to a Bitmap node
    int dest_bitmap_live;   // ... and that Bitmap carries a renderable buffer
    // GETBUFFERINFO's four source words, present only for that command. They
    // let the form classifier distinguish a valid calculation from the
    // M2 PARAMERROR width boundary without touching guest memory.
    int32_t mpeg_fmv_width;
    int32_t mpeg_fmv_height;
    int32_t mpeg_still_width;
    // TimerDriver's immediate usec commands read a signed TimeVal payload;
    // these words are populated only when the guest send buffer is readable.
    int32_t timer_seconds;
    int32_t timer_microseconds;
} m2_device_argument_view;

// The documented `ioi_CmdOptions` facts for one MPEGV WRITE/READ form
// (`mpVideoDriver.c:560 CheckOptions`). A null options pointer is valid.
typedef struct m2_device_options_view {
    int present;
    int aligned;
    int readable;   // readable for M2_DEVCMD_FMV_OPTIONS_SIZE bytes
    uint32_t reserved2;
    uint32_t reserved3;
} m2_device_options_view;

// MPEGVIDEOCMD_CONTROL's documented `CODECDeviceStatus` TagArg list
// (`device/mpegvideo.h:130`). The buffer is `DeviceStatus` (40 bytes) followed
// by `kMAX_CODEC_ARGS` (20) `TagArg` records of 8 bytes = 200 bytes, which is
// exactly the send length the recovered `FN_FMVDoControlCmd__6503c` authors.
#define M2_DEVCTL_MAX_TAGS          20u
#define M2_DEVCTL_CODEC_STATUS_SIZE 200u

// Tag ids. `TAG_ITEM_LAST == TAG_ITEM_RESERVED9 == 9` (`kernel/item.h:65-77`)
// and the codec tags start at `TAG_ITEM_LAST+1` with the header's four
// `VRESERVED*` placeholders in place, so the ids are derived rather than
// guessed. `TAG_END == 0` terminates the list (`kernel/types.h:104`).
#define M2_DEVCTL_TAG_END       0u
#define M2_DEVCTL_TAG_RESERVED0 10u
#define M2_DEVCTL_TAG_HSIZE     11u
#define M2_DEVCTL_TAG_VSIZE     12u
#define M2_DEVCTL_TAG_DEPTH     13u
#define M2_DEVCTL_TAG_DITHER    14u
#define M2_DEVCTL_TAG_STANDARD  15u
#define M2_DEVCTL_TAG_RESERVED1 16u
#define M2_DEVCTL_TAG_PLAY      17u
#define M2_DEVCTL_TAG_SKIPFRAMES 18u
#define M2_DEVCTL_TAG_KEYFRAMES 19u
#define M2_DEVCTL_TAG_RESERVED2 20u
#define M2_DEVCTL_TAG_RESERVED3 21u
#define M2_DEVCTL_TAG_YUVMODE   22u
#define M2_DEVCTL_TAG_RGBMODE   23u
#define M2_DEVCTL_TAG_M2MODE    24u

// Exact authored still setup in MPS_InitMPEGStill
// (generated/game_text_02.c:2031c-2038c). The host's still decoder already
// consumes this fixed 576x432 RGB555 contract, so this one complete TagArg list
// is a modeled no-op. Other control lists keep their per-tag disposition.
#define M2_DEVCTL_MPS_STILL_CONTROL_PC 0x0002038cu
#define M2_DEVCTL_MPS_STILL_STANDARD    0u
#define M2_DEVCTL_MPS_STILL_DEPTH       16u
#define M2_DEVCTL_MPS_STILL_HSIZE       576u
#define M2_DEVCTL_MPS_STILL_VSIZE       432u
#define M2_DEVCTL_MPS_STILL_M2MODE      0u
#define M2_DEVCTL_MPS_STILL_KEYFRAMES   0u

typedef struct m2_device_control_view {
    uint32_t tag[M2_DEVCTL_MAX_TAGS];
    uint32_t tag_arg[M2_DEVCTL_MAX_TAGS]; // TagArg.ta_Arg at +4
    uint32_t tag_count;     // entries before TAG_END, or M2_DEVCTL_MAX_TAGS
    uint32_t send_buffer;
    uint32_t send_len;
    uint32_t mem_size;
    uint32_t selector_pc;   // recovered caller site
    int buffer_aligned;
} m2_device_control_view;

// Classify one accepted command's recovered argument form. Returns M2K_OK when
// the form is one the host models, otherwise the documented refusal identity.
// `*form_out` receives the matched form id (never NULL on return); the caller
// publishes the identity through the guest's own IOReq and completes.
int32_t m2_device_argument_form_check(const m2_device_argument_view* view,
                                      const m2_device_options_view* options,
                                      const char** form_out);

// Classify the documented MPEGVIDEOCMD_CONTROL tag list the same way.
int32_t m2_device_control_form_check(const m2_device_control_view* view,
                                     const char** form_out);

// The documented DeviceStatus skeleton `mpvCmdStatus` writes for a device the
// host models but does not have a driver identity for: zeroed, with the
// structural fields set from the documented contract (a status size of
// M2_DEVCMD_STATUS_SIZE and a device block size of zero, which
// `CMD_BLOCKREAD`'s documentation defines as "not a block-oriented device").
// The identity/version/family fields are preserved unknowns and stay zero.
void m2_device_status_record_write(uint8_t* dst, uint32_t len,
                                   uint32_t* actual_out);

// File Folio `FileDriverStatus` record for the host's read-only disc-file
// boundary. The M2 implementation copies min(requested, sizeof(FileStatus)); it
// does not impose MPEG video's eight-byte CMD_STATUS minimum.
void m2_file_status_record_write(uint8_t* dst, uint32_t len,
                                 uint32_t file_bytes, uint32_t block_size,
                                 uint32_t* actual_out);

#ifdef __cplusplus
}
#endif

#endif // M2_DEVICE_COMMAND_CONTRACT_H
