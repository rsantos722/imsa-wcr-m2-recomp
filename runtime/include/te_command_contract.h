// Triangle Engine device-command accepted set.
//
// The vocabulary for this interface is the documented M2 driver interface
// (the documented Triangle Engine command set and device header). Every documented
// command, option bit, argument form and error identity lives here so that
// the command-contract audit and tests pin the decisions.
//
// The invariant this exists to enforce: a documented command form is either
// implemented and tested, or refused with a named identity — never silently
// completed with success. The host's generic send path (m2k_send_io) is
// command-agnostic and completes *any* request with M2K_OK, so an unmodelled
// command that is not explicitly refused reports success to the guest while
// doing nothing.
#ifndef M2_TE_COMMAND_CONTRACT_H
#define M2_TE_COMMAND_CONTRACT_H

#include <stdint.h>

#include "guest_kernel.h" // M2K_OK

// Command ids: MakeDeviceCmd(CMDC_TE, n) == ((CMDC_TE) << 16) | n with
// CMDC_TE == 0x00d. The M2 driver's `cmdTable`
// declares all nine documented commands and compiles the GETIDLETIME entry out
// unless BUILD_DEBUGGER is defined.
#define M2_TE_CMD_EXECUTELIST        0x000d0003u
#define M2_TE_CMD_SETVIEW            0x000d0004u
#define M2_TE_CMD_SETZBUFFER         0x000d0005u
#define M2_TE_CMD_SETFRAMEBUFFER     0x000d0006u
#define M2_TE_CMD_DISPLAYFRAMEBUFFER 0x000d0007u
#define M2_TE_CMD_STEP               0x000d0008u
#define M2_TE_CMD_SPEEDCONTROL       0x000d0009u
#define M2_TE_CMD_SETVBLABORTCOUNT   0x000d000au
#define M2_TE_CMD_GETIDLETIME        0x000d0100u

// TE_CMD_EXECUTELIST accepts exactly this option mask; the driver's own option
// switch has no default arm, so any other bit is TE_ERR_BADOPTIONS.
//   TE_CLEAR_FRAME_BUFFER  0x01   (documented; host has no consumer)
//   TE_CLEAR_Z_BUFFER      0x02   (host: te_z_fill_word -> clt_clear_z)
//   TE_WAIT_UNTIL_OFFSCREEN 0x04  (documented; never set by gstate.c)
//   TE_ABORT_AT_VBLANK     0x08   (documented; only GS_SetAbortVblank sets it)
//   TE_LIST_FLUSHED        0x10   (host: accepted, already-submitted semantics)
#define M2_TE_EXEC_OPTIONS_MASK 0x0000001fu

// TE_CMD_SPEEDCONTROL start-argument forms. SMALLEST_SPEEDBUMP is the driver's
// own floor: an interval below it is raised to the floor, not rejected.
#define M2_TE_FULLSPEED          0x00000000u
#define M2_TE_STOPPED            0xffffffffu
#define M2_TE_SMALLEST_SPEEDBUMP 5u

// TE device error identities. MakeTEErr(ER_SEVERE, class, err) is
// MAKEERR(ER_DEVC, ER_TE, ER_SEVERE, ER_E_SSTM, class, err) with
// ER_DEVC = Make6Bit('D') = 40 and ER_TE = MakeErrId('T','E') = 0xe29, giving
// the recovered base 0xd1c53000. The same construction reproduces the
// the project's GK_GFX_ERR_BADITEM (0xd55b9001) exactly, so these are derived from
// the M2 interface's error-macro definitions rather than guessed.
#define M2_TE_ERR_BADITEM    0xd1c53001u // STND, ER_BadItem = 1
#define M2_TE_ERR_BADIOARG   0xd1c5300du // STND, ER_BadIOArg = 13
#define M2_TE_ERR_NOLIST     0xd1c53103u // NSTND, 3
#define M2_TE_ERR_BADOPTIONS 0xd1c53104u // NSTND, 4
#define M2_TE_ERR_BADBITMAP  0xd1c53105u // NSTND, 5
#define M2_TE_ERR_NOFRAMEBUFFER 0xd1c53106u // NSTND, 6
#define M2_TE_ERR_NOTPAUSED  0xd1c53107u // NSTND, 7
#define M2_TE_ERR_NOVIEW     0xd1c53108u // NSTND, 8
#define M2_TE_ERR_BADCOMMAND 0xd57b900cu // kernel STND ER_BadCommand = 12

// Host-assigned code inside the TE identity space, never emitted by the
// M2 driver: a documented command whose device state the host does not
// model is refused rather than silently completed. Named in the recompilation
// limitations as "Unmodelled TE device commands".
#define M2_TE_ERR_UNMODELLED 0xd1c53110u

// TE_CMD_EXECUTELIST request refusal. Returns M2K_OK to accept, or the driver's
// identity for a request the device refuses before examining the list:
//   * send_buffer == NULL                     -> M2_TE_ERR_NOLIST
//   * command_options outside the 0x1f mask   -> M2_TE_ERR_BADOPTIONS
// A zero-length list with a live buffer is *not* a refusal: it is the
// clear-Z-only form (see `empty_te_clear` in recomp_runtime.c).
int32_t m2_te_execute_list_reject(uint32_t send_buffer,
                                  uint32_t command_options);

// Node-type vs Bitmap-attribute split used by TE_CMD_SETZBUFFER and
// TE_CMD_SETFRAMEBUFFER: a non-Bitmap I/O offset is TE_ERR_BADITEM, while a
// Bitmap of the wrong type, with no buffer/size, or without BMF_RENDERABLE is
// TE_ERR_BADBITMAP.
int32_t m2_te_bitmap_argument_error(int is_bitmap_node, int bitmap_ok);

// Result of a documented TE control command the host does not model.
// *handled = 1 when `command` is a recognized control command:
//   * M2K_OK               -> the documented form is a genuine no-op for this
//                             host (TE_CMD_SPEEDCONTROL: TE_FULLSPEED); the
//                             request completes through the normal send path.
//   * M2_TE_ERR_NOTPAUSED  -> TE_CMD_STEP, which the driver itself can only
//                             honour while the device is in speed-bump control.
//   * M2_TE_ERR_BADIOARG  -> TE_CMD_SETVBLABORTCOUNT with a negative ioi_Offset,
//                            which the M2 driver rejects before dispatch.
//   * M2_TE_ERR_BADCOMMAND -> GETIDLETIME in the shipped non-debug M2 driver,
//                             whose command table omits that debug-only entry.
//   * M2_TE_ERR_UNMODELLED -> any other documented form (throttle request or
//                             nonnegative VBL-abort count):
//                             refused instead of silently succeeding.
// *handled = 0 for every command outside this set.
int32_t m2_te_control_command_result(uint32_t command,
                                     uint32_t command_options,
                                     uint32_t command_offset,
                                     int* handled);

#endif // M2_TE_COMMAND_CONTRACT_H
