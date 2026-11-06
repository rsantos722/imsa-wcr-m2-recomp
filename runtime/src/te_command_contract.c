// TE device-command accepted set — see include/te_command_contract.h.
//
// Pure decisions only: no guest memory is read and no host state is touched.
// `recomp_runtime.c` supplies the IOInfo words and acts on the result, which
// keeps the production wiring reviewable in a few lines.
#include "te_command_contract.h"

int32_t m2_te_execute_list_reject(uint32_t send_buffer,
                                  uint32_t command_options) {
    // driver.c CmdExecuteList refuses a NULL buffer before it looks at options.
    if (send_buffer == 0u)
        return (int32_t)M2_TE_ERR_NOLIST;
    if ((command_options & ~M2_TE_EXEC_OPTIONS_MASK) != 0u)
        return (int32_t)M2_TE_ERR_BADOPTIONS;
    return M2K_OK;
}

int32_t m2_te_bitmap_argument_error(int is_bitmap_node, int bitmap_ok) {
    if (!is_bitmap_node)
        return (int32_t)M2_TE_ERR_BADITEM;
    if (!bitmap_ok)
        return (int32_t)M2_TE_ERR_BADBITMAP;
    return M2K_OK;
}

int32_t m2_te_control_command_result(uint32_t command,
                                     uint32_t command_options,
                                     uint32_t command_offset, int* handled) {
    if (handled)
        *handled = 1;
    switch (command) {
    case M2_TE_CMD_STEP:
        // CmdSingleStep is only legal while the device runs under speed-bump
        // control (gs_Speed >= SMALLEST_SPEEDBUMP, or TE_STOPPED). This host
        // delivers every list immediately and never throttles the device, so
        // the released driver's own answer for every reachable call is
        // TE_ERR_NOTPAUSED.
        return (int32_t)M2_TE_ERR_NOTPAUSED;
    case M2_TE_CMD_SPEEDCONTROL:
        // TE_FULLSPEED clears a throttle this host never applies: the guest's
        // gs_Speed bookkeeping is guest state and nothing device-visible
        // changes, so the request completes normally. This is also the only
        // form the image authors (gstate.c's init path).
        if (command_options == M2_TE_FULLSPEED)
            return M2K_OK;
        // TE_STOPPED, a speed-bump interval at or above the driver's
        // SMALLEST_SPEEDBUMP floor, and the 1..4 sub-floor values the driver
        // raises to that floor: honouring any of them needs per-list pacing the
        // host does not implement, and completing with M2K_OK would tell the
        // caller the device is throttled when it is not.
        if (command_options == M2_TE_STOPPED ||
            command_options >= M2_TE_SMALLEST_SPEEDBUMP)
            return (int32_t)M2_TE_ERR_UNMODELLED;
        return (int32_t)M2_TE_ERR_UNMODELLED;
    case M2_TE_CMD_SETVBLABORTCOUNT:
        // CmdSetVBLAbortCount checks the signed IOInfo offset before calling
        // DispatchTEIO. Preserve that deterministic source error even though
        // the host does not model the later VBL-abort scheduler.
        if ((int32_t)command_offset < 0)
            return (int32_t)M2_TE_ERR_BADIOARG;
        return (int32_t)M2_TE_ERR_UNMODELLED;
    case M2_TE_CMD_GETIDLETIME:
        // The released cmdTable compiles GETIDLETIME out unless BUILD_DEBUGGER
        // is defined. The preserved retail configuration therefore takes the
        // device dispatcher's ordinary BADCOMMAND path; no guessed idle-time
        // accumulator is involved.
        return (int32_t)M2_TE_ERR_BADCOMMAND;
    default:
        break;
    }
    if (handled)
        *handled = 0;
    return M2K_OK;
}
